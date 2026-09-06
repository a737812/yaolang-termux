/* ════════════════════════════════════════════════════════════════════════════
 * 曜语 (YaoLang) 编译器 —— 源代码本体
 * 版本: 0.1.0-alpha  曜历元年
 *
 * 设计目标：
 *   - 关键字、语法符号 100% 中文，词法阶段直接拒绝英文关键字
 *   - 变量名/成员名 允许 中文/英文/数字（标识符不限制）
 *   - 静态类型、所有权内存模型、零成本抽象
 *   - 编译到原生机器码（通过 C 中间层 → 系统 C 编译器）
 *
 * 架构：
 *   源码.yao → [词法] → Token流 → [语法] → AST → [语义/类型] → C代码 → [系统cc] → 可执行文件
 *
 * 作者: 曜神 (由 Gemini 构造)
 * 许可: MIT
 * ════════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

/* ──────────────────────────── 宏 & 全局 ──────────────────────────── */

#define YVM_VERSION "0.1.0-alpha"
#define MAX_TOKEN_LEN   4096
#define MAX_LINE_LEN    8192
#define MAX_ERRORS      64
#define MAX_INCLUDES    64
#define MAX_AST_CHILDREN 64
#define MAX_SYMBOLS     4096
#define MAX_TYPES       512
#define MAX_CODEGEN_BUF (16 * 1024 * 1024)  // 16 MB
#define MAX_SCOPES      256
#define MAX_STRUCT_MEMBERS 256
#define MAX_FN_PARAMS    32
#define MAX_ENUM_VARIANTS 256
#define MAX_TRAIT_METHODS 64
#define MAX_IMPL_ITEMS    128

#define YAO_EXT_YAO  ".yao"

/* 曜语源文件扩展名检查 */
static bool is_yao_file(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    return strcmp(ext, ".yao") == 0 ||
           strcmp(ext, ".\xe8\x80\x80") == 0; /* .yao 或 .耀(U+8000) */
}

/* ──────────────────────────── 编译阶段标记 ──────────────────────────── */
typedef enum {
    PHASE_LEX,
    PHASE_PARSE,
    PHASE_SEMA,
    PHASE_CGEN,
    PHASE_LINK
} CompilePhase;

/* ──────────────────────────── 错误处理 ──────────────────────────── */
typedef struct {
    CompilePhase phase;
    char file[256];
    int  line;
    int  col;
    char msg[1024];
} CompileError;

static CompileError g_errors[MAX_ERRORS];
static int g_error_count = 0;
static bool g_had_error = false;
static const char *g_current_file = "<unknown>";

static void yao_error(CompilePhase ph, int line, int col, const char *fmt, ...) {
    if (g_error_count >= MAX_ERRORS) return;
    CompileError *e = &g_errors[g_error_count++];
    e->phase = ph;
    e->line = line;
    e->col  = col;
    strncpy(e->file, g_current_file, sizeof(e->file) - 1);
    va_list ap; va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
    g_had_error = true;
}

static void yao_error_at_current(CompilePhase ph, const char *fmt, ...);

static int print_errors(void) {
    int n = 0;
    for (int i = 0; i < g_error_count; i++) {
        CompileError *e = &g_errors[i];
        const char *phase_name[] = {"词法分析", "语法分析", "语义分析", "代码生成", "链接"};
        fprintf(stderr, "曜语编译错误 [阶段:%s] %s:%d:%d: %s\n",
                phase_name[e->phase], e->file, e->line, e->col, e->msg);
        n++;
    }
    return n;
}

/* ──────────────────────────── 动态字符串 ──────────────────────────── */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb) {
    sb->cap = 256;
    sb->data = (char *)malloc(sb->cap);
    sb->len = 0;
    sb->data[0] = '\0';
}

static void sb_ensure(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 > sb->cap) {
        while (sb->len + extra + 1 > sb->cap) sb->cap *= 2;
        sb->data = (char *)realloc(sb->data, sb->cap);
    }
}

static void sb_append(StrBuf *sb, const char *s) {
    size_t n = strlen(s);
    sb_ensure(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_append_char(StrBuf *sb, char c) {
    sb_ensure(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

static void sb_appendf(StrBuf *sb, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[8192];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sb_append(sb, buf);
    (void)n;
}

static void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

/* ──────────────────────────── UTF-8 工具 ──────────────────────────── */

/* 读取一个 UTF-8 字符，返回字节长度, 写入 *cp */
static int utf8_decode(const char *s, int32_t *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0) {
        *cp = ((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        *cp = ((c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) | ((unsigned char)s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        *cp = ((c & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) |
              (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F);
        return 4;
    }
    *cp = '?';
    return 1;
}

/* 判断一个 codepoint 是否为中文字符（CJK统一汉字区 + 扩展A） */
static bool is_cjk(int32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||   /* CJK统一汉字 */
           (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK扩展A */
           (cp >= 0xF900 && cp <= 0xFAFF);      /* CJK兼容汉字 */
}

/* 判断标识符首字符: 中文字、英文字母、下划线 */
static bool is_ident_start_cp(int32_t cp) {
    return is_cjk(cp) ||
           (cp >= 'a' && cp <= 'z') ||
           (cp >= 'A' && cp <= 'Z') ||
           cp == '_';
}

/* 判断标识符后续: 首字符集 + 数字 + 少数符号 */
static bool is_ident_cont_cp(int32_t cp) {
    return is_ident_start_cp(cp) ||
           (cp >= '0' && cp <= '9');
}

/* 是否ASCII标识符首字符 */
static bool is_ident_start_ascii(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/* 是否ASCII标识符后续字符 */
static bool is_ident_cont_ascii(char c) {
    return is_ident_start_ascii(c) || (c >= '0' && c <= '9');
}

/* ════════════════════════════════════════════════════════════════════
 *  词法分析器
 * ════════════════════════════════════════════════════════════════════ */

/* ──────────────────────────── Token 类型 ──────────────────────────── */
typedef enum {
    /* 字面值 */
    TK_INT,        // 整数字面值
    TK_FLOAT,      // 浮点字面值
    TK_STRING,     // 字符串字面值
    TK_CHAR,       // 字符字面值 'x'
    TK_BOOL_TRUE,  // 真
    TK_BOOL_FALSE, // 假

    /* 标识符 (变量名/函数名/类名 等允许中英文) */
    TK_IDENT,

    /* ── 关键字（全中文，锁死） ── */
    TK_KW_FN,        // 函数
    TK_KW_VAR,       // 变量 (可变)
    TK_KW_LET,       // 令 (不可变绑定)
    TK_KW_MUT,       // 可变 (修饰)
    TK_KW_CONST,     // 常量
    TK_KW_IF,        // 若
    TK_KW_ELSE,     // 否则
    TK_KW_ELSE_IF,   // 否则若
    TK_KW_WHILE,     // 当
    TK_KW_LOOP,      // 循环
    TK_KW_FOR,       // 对于
    TK_KW_IN,        // 在
    TK_KW_BREAK,    // 跳出
    TK_KW_CONTINUE, // 继续
    TK_KW_RETURN,   // 返回
    TK_KW_MATCH,    // 匹配
    TK_KW_STRUCT,   // 结构
    TK_KW_ENUM,      // 枚举
    TK_KW_TRAIT,    // 特质
    TK_KW_IMPL,     // 实现
    TK_KW_PUB,      // 公开
    TK_KW_PRIV,     // 私有
    TK_KW_SELF,     // 自身
    TK_KW_NEW,      // 新建
    TK_KW_DEL,      // 删除
    TK_KW_AS,       // 作为
    TK_KW_USE,      // 引入 (use / import)
    TK_KW_MOD,      // 模块
    TK_KW_TYPE,     // 类型别名
    TK_KW_REF,      // 引用
    TK_KW_MOVE,     // 转移 (move 语义)
    TK_KW_BORROW,   // 借用
    TK_KW_UNSAFE,   // 不安全
    TK_KW_EXTERN,   // 外部
    TK_KW_ASYNC,    // 异步
    TK_KW_AWAIT,    // 等待
    TK_KW_THROW,    // 抛出
    TK_KW_TRY,      // 尝试
    TK_KW_CATCH,    // 捕获
    TK_KW_STATIC,   // 静态
    TK_KWNIL,       // 空 (null / nil)
    TK_KW_SIZEOF,   // 取大小
    TK_KW_BOX,      // 装箱 (heap alloc)

    /* ── 类型关键字 ── */
    TK_TYPE_INT,     // 整数
    TK_TYPE_FLOAT,   // 浮点
    TK_TYPE_BOOL,    // 布尔
    TK_TYPE_STRING,  // 文本
    TK_TYPE_CHAR,    // 字符
    TK_TYPE_VOID,    // 空类型

    /* ── 运算符与分隔符 ── */
    TK_PLUS,        // 加
    TK_PLUS_EQ,     // 加等
    TK_MINUS,       // 减
    TK_MINUS_EQ,    // 减等
    TK_STAR,        // 乘号 (注意: 星号 * 虽是ASCII但作为运算符允许)
    TK_STAR_EQ,
    TK_SLASH,       // 除号 (/)
    TK_SLASH_EQ,
    TK_PERCENT,     // 取模 (%)
    TK_PERCENT_EQ,
    TK_ASSIGN,      // 赋值号 (注意: = 是ASCII运算符)
    TK_EQ_EQ,       // 相等比较
    TK_BANG,        // 非 (!)
    TK_BANG_EQ,     // 不等 !=
    TK_LT,          // 小于
    TK_LE,          // 小于等于 <=
    TK_GT,          // 大于
    TK_GE,          // 大于等于 >=
    TK_AND,         // 且 &&
    TK_OR,          // 或 ||
    TK_BIT_AND,     // 位与 &
    TK_BIT_OR,      // 位或 |
    TK_BIT_XOR,      // 位异或 ^
    TK_BIT_NOT,      // 位取反 ~
    TK_SHL,          // 左移 <<
    TK_SHR,          // 右移 >>
    TK_ARROW,        // 返回箭头 →
    TK_FAT_ARROW,    // 大箭头 => (match)
    TK_DOT,          // 成员访问 .
    TK_DOTDOT,       // 范围 ..
    TK_DOTDOTDOT,   // 可变参数 ...
    TK_COMMA,        // 逗号,
    TK_COLON,        // 冒号:
    TK_COLONCOLON,   // 双冒号:: (路径)
    TK_SEMICOLON,    // 分号;  (语句终结)
    TK_LPAREN,       // (
    TK_RPAREN,       // )
    TK_LBRACE,       // {
    TK_RBRACE,       // }
    TK_LBRACKET,     // [
    TK_RBRACKET,     // ]
    TK_QUESTION,     // 问号 ? (Option类型)

    /* ── 中文运算符号 ── */
    TK_CN_PLUS,      // ＋ (全角加号) -> plus
    TK_CN_MINUS,     // － -> minus
    TK_CN_STAR,      // × (中文乘号)
    TK_CN_SLASH,     // ÷ (中文除号)
    TK_CN_ASSIGN,    // ← (赋值箭头, 曦特色)
    TK_CN_EQ,        // ＝ (全角等号)
    TK_CN_NEQ,       // ≠ (不等于)
    TK_CN_LT,        // ＜ (全角小于)
    TK_CN_LE,        // ≤ (小于等于)
    TK_CN_GT,        // ＞ (全角大于)
    TK_CN_GE,        // ≥ (大于等于)
    TK_CN_AND,       // ∧ (逻辑与)
    TK_CN_OR,        // ∨ (逻辑或)
    TK_CN_NOT,       // ¬ (逻辑非)
    TK_CN_MEMBER,    // · (成员访问符, 中缀点)
    TK_CN_ARROW,     // → (中文箭头)

    TK_EOF,
    TK_INVALID,
} TokenType;

typedef struct {
    TokenType type;
    char      text[MAX_TOKEN_LEN];  // 原始文本
    int64_t   int_val;               // 整数值
    double    float_val;             // 浮点值
    int       line;
    int       col;
    const char *file;
} Token;

/* ──────────────────────────── 关键字表 ──────────────────────────── */
typedef struct {
    const char *text;   // UTF-8 字符串
    TokenType   type;
} KwEntry;

/* 全中文关键字表 — 词法阶段只匹配这些 */
static const KwEntry g_keywords[] = {
    /* 控制流 */
    {"若",      TK_KW_IF},
    {"否则若",  TK_KW_ELSE_IF},
    {"否则",    TK_KW_ELSE},
    {"当",      TK_KW_WHILE},
    {"循环",    TK_KW_LOOP},
    {"对于",    TK_KW_FOR},
    {"在",      TK_KW_IN},
    {"跳出",    TK_KW_BREAK},
    {"继续",    TK_KW_CONTINUE},
    {"返回",    TK_KW_RETURN},
    {"匹配",    TK_KW_MATCH},

    /* 声明 */
    {"函数",    TK_KW_FN},
    {"变量",    TK_KW_VAR},
    {"令",      TK_KW_LET},
    {"可变",    TK_KW_MUT},
    {"常量",    TK_KW_CONST},
    {"结构",    TK_KW_STRUCT},
    {"枚举",    TK_KW_ENUM},
    {"特质",    TK_KW_TRAIT},
    {"实现",    TK_KW_IMPL},
    {"公开",    TK_KW_PUB},
    {"私有",    TK_KW_PRIV},
    {"引入",    TK_KW_USE},
    {"模块",    TK_KW_MOD},
    {"类型",    TK_KW_TYPE},
    {"静态",    TK_KW_STATIC},
    {"外部",    TK_KW_EXTERN},

    /* 内存/引用 */
    {"引用",    TK_KW_REF},
    {"借用",    TK_KW_BORROW},
    {"转移",    TK_KW_MOVE},
    {"不安全",  TK_KW_UNSAFE},
    {"装箱",    TK_KW_BOX},

    /* 面向对象 */
    {"自身",    TK_KW_SELF},
    {"新建",    TK_KW_NEW},

    /* 异常 */
    {"抛出",    TK_KW_THROW},
    {"尝试",    TK_KW_TRY},
    {"捕获",    TK_KW_CATCH},

    /* 异步 */
    {"异步",    TK_KW_ASYNC},
    {"等待",    TK_KW_AWAIT},

    /* 杂项 */
    {"空",      TK_KWNIL},
    {"作为",    TK_KW_AS},
    {"取大小",  TK_KW_SIZEOF},

    /* 类型关键字 (也是中文) */
    {"整数",    TK_TYPE_INT},
    {"浮点",    TK_TYPE_FLOAT},
    {"布尔",    TK_TYPE_BOOL},
    {"文本",    TK_TYPE_STRING},
    {"字符",    TK_TYPE_CHAR},
    {"空类型",  TK_TYPE_VOID},

    /* 布尔字面值 */
    {"真",      TK_BOOL_TRUE},
    {"假",      TK_BOOL_FALSE},
};

static const int g_keyword_count = sizeof(g_keywords) / sizeof(g_keywords[0]);

/* 英文关键字黑名单 — 词法阶段直接拒绝 */
static const char *g_english_blacklist[] = {
    "if", "else", "for", "while", "loop", "break", "continue",
    "return", "fn", "func", "function", "let", "var", "const",
    "match", "switch", "case", "struct", "enum", "trait", "impl",
    "pub", "priv", "use", "import", "mod", "module", "type",
    "ref", "move", "borrow", "unsafe", "extern", "async", "await",
    "throw", "try", "catch", "nil", "null", "none", "void",
    "int", "float", "bool", "bool", "string", "char", "double",
    "self", "this", "new", "delete", "sizeof", "box",
    "true", "false", "as", "in", "where", "with",
    "class", "interface", "extends", "implements", "abstract",
    "static", "final", "public", "private", "protected", "internal",
    "do", "end", "begin", "then", "when", "yield", "defer",
    "go", "chan", "select", "package", "func", "type", "range",
    "and", "or", "not", "is", "isnot", "lambda", "yield",
    NULL
};

/* 检查一个纯ASCII标识符是否命中黑名单 */
static bool is_english_keyword(const char *s) {
    for (int i = 0; g_english_blacklist[i]; i++) {
        if (strcmp(s, g_english_blacklist[i]) == 0) return true;
    }
    return false;
}

/* 全角/中文运算符号表 */
static const KwEntry g_cn_operators[] = {
    {"＋",  TK_CN_PLUS},
    {"－",  TK_CN_MINUS},
    {"×",  TK_CN_STAR},
    {"÷",  TK_CN_SLASH},
    {"←",  TK_CN_ASSIGN},
    {"＝",  TK_CN_EQ},
    {"≠",  TK_CN_NEQ},
    {"＜",  TK_CN_LT},
    {"≤",  TK_CN_LE},
    {"＞",  TK_CN_GT},
    {"≥",  TK_CN_GE},
    {"∧",  TK_CN_AND},
    {"∨",  TK_CN_OR},
    {"¬",  TK_CN_NOT},
    {"·",  TK_CN_MEMBER},
    {"→",  TK_CN_ARROW},
    {"⇒",  TK_FAT_ARROW},
};
static const int g_cn_op_count = sizeof(g_cn_operators) / sizeof(g_cn_operators[0]);

/* ──────────────────────────── Lexer 状态 ──────────────────────────── */
typedef struct {
    const char *src;       // 源代码
    size_t      src_len;
    size_t      pos;       // 当前字节位置
    int         line;
    int         col;
    const char *file;
    Token      *tokens;    // token 数组
    int         token_count;
    int         token_cap;
} Lexer;

static void lex_add_token(Lexer *lx, Token tok) {
    if (lx->token_count >= lx->token_cap) {
        lx->token_cap = lx->token_cap ? lx->token_cap * 2 : 256;
        lx->tokens = (Token *)realloc(lx->tokens, sizeof(Token) * lx->token_cap);
    }
    lx->tokens[lx->token_count++] = tok;
}

static Token lex_make_token(Lexer *lx, TokenType type, const char *text) {
    Token t = {0};
    t.type = type;
    t.line = lx->line;
    t.col  = lx->col;
    t.file = lx->file;
    strncpy(t.text, text, MAX_TOKEN_LEN - 1);
    return t;
}

/* 跳过空白和注释 */
static void lex_skip_ws(Lexer *lx) {
    while (lx->pos < lx->src_len) {
        unsigned char c = (unsigned char)lx->src[lx->pos];
        // ASCII 空白
        if (c == ' ' || c == '\t' || c == '\r') { lx->pos++; lx->col++; continue; }
        if (c == '\n') { lx->pos++; lx->line++; lx->col = 1; continue; }

        // 行注释: 双井号 ## 或 // 曜语默认用 ## (中文习惯)
        if (c == '#' && lx->pos + 1 < lx->src_len && lx->src[lx->pos + 1] == '#') {
            while (lx->pos < lx->src_len && lx->src[lx->pos] != '\n') lx->pos++;
            continue;
        }
        // 兼容 //
        if (c == '/' && lx->pos + 1 < lx->src_len && lx->src[lx->pos + 1] == '/') {
            while (lx->pos < lx->src_len && lx->src[lx->pos] != '\n') lx->pos++;
            continue;
        }
        // 块注释: #* ... *#  ( 曜语风格 )
        if (c == '#' && lx->pos + 1 < lx->src_len && lx->src[lx->pos + 1] == '*') {
            lx->pos += 2;
            int depth = 1;
            while (lx->pos < lx->src_len && depth > 0) {
                if (lx->src[lx->pos] == '#' && lx->pos + 1 < lx->src_len && lx->src[lx->pos+1] == '*') {
                    depth++; lx->pos += 2;
                } else if (lx->src[lx->pos] == '*' && lx->pos + 1 < lx->src_len && lx->src[lx->pos+1] == '#') {
                    depth--; lx->pos += 2;
                } else {
                    if (lx->src[lx->pos] == '\n') { lx->line++; lx->col = 1; }
                    lx->pos++;
                }
            }
            continue;
        }
        break;
    }
}

/* 尝试匹配中文关键字或运算符 */
static bool lex_try_keyword(const char *src, size_t pos, size_t len, const char *kw) {
    size_t kl = strlen(kw);
    if (pos + kl > len) return false;
    return memcmp(src + pos, kw, kl) == 0;
}

/* 尝试匹配 UTF-8 字符串前缀 */
static size_t lex_match_utf8(const char *src, size_t pos, size_t len, const char *kw) {
    size_t kl = strlen(kw);
    if (pos + kl > len) return 0;
    if (memcmp(src + pos, kw, kl) == 0) return kl;
    return 0;
}

/* 词法分析核心 */
static void lex(Lexer *lx) {
    while (lx->pos < lx->src_len) {
        lex_skip_ws(lx);
        if (lx->pos >= lx->src_len) break;

        int start_line = lx->line;
        int start_col  = lx->col;
        size_t start_pos = lx->pos;
        unsigned char c = (unsigned char)lx->src[lx->pos];

        /* ── 1. 整数和浮点数 ── */
        if (c >= '0' && c <= '9') {
            char buf[MAX_TOKEN_LEN];
            int bi = 0;
            bool is_float = false;
            while (lx->pos < lx->src_len && ((lx->src[lx->pos] >= '0' && lx->src[lx->pos] <= '9')
                   || lx->src[lx->pos] == '.')) {
                if (lx->src[lx->pos] == '.') {
                    // 检查后面是否是数字（排除范围运算符 .. )
                    if (lx->pos + 1 < lx->src_len && lx->src[lx->pos+1] == '.') break;
                    if (lx->pos + 1 < lx->src_len && !(lx->src[lx->pos+1] >= '0' && lx->src[lx->pos+1] <= '9')) break;
                    is_float = true;
                }
                buf[bi++] = lx->src[lx->pos++];
                lx->col++;
            }
            buf[bi] = '\0';
            Token t = lex_make_token(lx, is_float ? TK_FLOAT : TK_INT, buf);
            if (is_float) t.float_val = atof(buf);
            else t.int_val = strtoll(buf, NULL, 10);
            t.line = start_line; t.col = start_col;
            lex_add_token(lx, t);
            continue;
        }

        /* ── 2. 字符串字面值 ── */
        if (c == '"' || c == '\'') {
            /* 曜语同时支持 " 和 「」 作为字符串 */
            char quote = (char)c;
            bool use_cn_bracket = false;
            if (c == '\'') {
                // 可能是中文字符串「...」或字符'x'
                // 先检查是否是「
                if (lx->pos + 2 < lx->src_len && (unsigned char)lx->src[lx->pos] == 0xE3
                    && (unsigned char)lx->src[lx->pos+1] == 0x80) {
                    // 简单处理: 不是中文括号, 按字符处理
                }
                // 简化: 单引号 = 字符
                lx->pos++;
                char ch = lx->src[lx->pos];
                lx->pos += 1;
                if (lx->src[lx->pos] == '\'') lx->pos++;
                char buf[8]; buf[0] = ch; buf[1] = '\0';
                Token t = lex_make_token(lx, TK_CHAR, buf);
                t.int_val = (unsigned char)ch;
                t.line = start_line; t.col = start_col;
                lex_add_token(lx, t);
                continue;
            }
            // 双引号字符串
            lx->pos++; lx->col++;
            StrBuf sb; sb_init(&sb);
            while (lx->pos < lx->src_len && lx->src[lx->pos] != '"') {
                if (lx->src[lx->pos] == '\\' && lx->pos + 1 < lx->src_len) {
                    lx->pos++; lx->col++;
                    char esc = lx->src[lx->pos];
                    switch (esc) {
                        case 'n':  sb_append_char(&sb, '\n'); break;
                        case 't':  sb_append_char(&sb, '\t'); break;
                        case 'r':  sb_append_char(&sb, '\r'); break;
                        case '\\': sb_append_char(&sb, '\\'); break;
                        case '"':  sb_append_char(&sb, '"');  break;
                        case '0':  sb_append_char(&sb, '\0');  break;
                        case '\'': sb_append_char(&sb, '\''); break;
                        default:   sb_append_char(&sb, esc); break;
                    }
                    lx->pos++; lx->col++;
                } else if (lx->src[lx->pos] == '\n') {
                    sb_append_char(&sb, '\n');
                    lx->pos++; lx->line++; lx->col = 1;
                } else {
                    sb_append_char(&sb, lx->src[lx->pos]);
                    lx->pos++; lx->col++;
                }
            }
            if (lx->pos < lx->src_len) { lx->pos++; lx->col++; } // closing "
            Token t = lex_make_token(lx, TK_STRING, sb.data);
            t.line = start_line; t.col = start_col;
            lex_add_token(lx, t);
            sb_free(&sb);
            (void)use_cn_bracket;
            continue;
        }

        /* ── 3. 中文字符串「」 ── */
        if (c == 0xE3 && lx->pos + 2 < lx->src_len
            && (unsigned char)lx->src[lx->pos+1] == 0x80
            && (unsigned char)lx->src[lx->pos+2] == 0x8C) {
            // 「 = U+300C = E3 80 8C
            lx->pos += 3; lx->col += 2;
            StrBuf sb; sb_init(&sb);
            while (lx->pos < lx->src_len) {
                // 」 = U+300D = E3 80 8D
                if ((unsigned char)lx->src[lx->pos] == 0xE3
                    && lx->pos + 2 < lx->src_len
                    && (unsigned char)lx->src[lx->pos+1] == 0x80
                    && (unsigned char)lx->src[lx->pos+2] == 0x8D) {
                    lx->pos += 3; lx->col += 2;
                    break;
                }
                if (lx->src[lx->pos] == '\n') { lx->line++; lx->col = 1; }
                else lx->col++;
                sb_append_char(&sb, lx->src[lx->pos]);
                lx->pos++;
            }
            Token t = lex_make_token(lx, TK_STRING, sb.data);
            t.line = start_line; t.col = start_col;
            lex_add_token(lx, t);
            sb_free(&sb);
            continue;
        }

        /* ── 4. 尝试匹配中文运算符 ── */
        {
            bool matched = false;
            for (int i = 0; i < g_cn_op_count; i++) {
                size_t m = lex_match_utf8(lx->src, lx->pos, lx->src_len, g_cn_operators[i].text);
                if (m > 0) {
                    Token t = lex_make_token(lx, g_cn_operators[i].type, g_cn_operators[i].text);
                    t.line = start_line; t.col = start_col;
                    lx->pos += m;
                    lx->col += 2; // 近似
                    lex_add_token(lx, t);
                    matched = true;
                    break;
                }
            }
            if (matched) continue;

            /* ── 5. 尝试匹配中文关键字 ── */
            for (int i = 0; i < g_keyword_count; i++) {
                size_t m = lex_match_utf8(lx->src, lx->pos, lx->src_len, g_keywords[i].text);
                if (m > 0) {
                    // 必须确定后面不是标识符继续字符
                    size_t after = lx->pos + m;
                    if (after < lx->src_len) {
                        int32_t cp_after;
                        int al = utf8_decode(&lx->src[after], &cp_after);
                        (void)al;
                        if (is_ident_cont_cp(cp_after)) continue; // 不是关键字, 是标识符的一部分
                    }
                    Token t = lex_make_token(lx, g_keywords[i].type, g_keywords[i].text);
                    t.line = start_line; t.col = start_col;
                    lx->pos += m;
                    lx->col += 2;
                    lex_add_token(lx, t);
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }

        /* ── 6. 标识符 (允许中文/英文/数字/下划线) ── */
        {
            int32_t cp;
            int cplen = utf8_decode(&lx->src[lx->pos], &cp);
            if (is_ident_start_cp(cp)) {
                char buf[MAX_TOKEN_LEN];
                int bi = 0;
                bool all_ascii = true;
                while (lx->pos < lx->src_len) {
                    int32_t c2;
                    int l2 = utf8_decode(&lx->src[lx->pos], &c2);
                    if (!is_ident_cont_cp(c2)) break;
                    if (!is_cjk(c2) && c2 > 0x7F) all_ascii = false;
                    for (int j = 0; j < l2; j++) buf[bi++] = lx->src[lx->pos + j];
                    lx->pos += l2;
                    lx->col += (c2 > 0x7F) ? 2 : 1;
                }
                buf[bi] = '\0';

                /* 🔒 核心安全检查: 纯ASCII标识符如果命中英文关键字黑名单 → 直接报错拒绝 */
                if (all_ascii && is_english_keyword(buf)) {
                    yao_error(PHASE_LEX, start_line, start_col,
                        "拒绝英文关键字 '%s'！ 曜语的关键字是中文。请使用对应的中文字钥。", buf);
                    Token t = lex_make_token(lx, TK_INVALID, buf);
                    t.line = start_line; t.col = start_col;
                    lex_add_token(lx, t);
                    continue;
                }
                Token t = lex_make_token(lx, TK_IDENT, buf);
                t.line = start_line; t.col = start_col;
                lex_add_token(lx, t);
                continue;
            }
            (void)cplen;
        }

        /* ── 7. ASCII 运算符和分隔符 ── */
        {
            char c1 = lx->src[lx->pos];
            char c2 = (lx->pos + 1 < lx->src_len) ? lx->src[lx->pos + 1] : 0;

            // 双字符运算符
            if (c1 == '=' && c2 == '=') {
                Token t = lex_make_token(lx, TK_EQ_EQ, "=="); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '!' && c2 == '=') {
                Token t = lex_make_token(lx, TK_BANG_EQ, "!="); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '<' && c2 == '=') {
                Token t = lex_make_token(lx, TK_LE, "<="); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '>' && c2 == '=') {
                Token t = lex_make_token(lx, TK_GE, ">="); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '<' && c2 == '<') {
                Token t = lex_make_token(lx, TK_SHL, "<<"); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '>' && c2 == '>') {
                Token t = lex_make_token(lx, TK_SHR, ">>"); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '&' && c2 == '&') {
                Token t = lex_make_token(lx, TK_AND, "&&"); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '|' && c2 == '|') {
                Token t = lex_make_token(lx, TK_OR, "||"); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '+' && c2 == '=') {
                Token t = lex_make_token(lx, TK_PLUS_EQ, "+="); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '-' && c2 == '=') {
                Token t = lex_make_token(lx, TK_MINUS_EQ, "-="); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '*' && c2 == '=') {
                Token t = lex_make_token(lx, TK_STAR_EQ, "*="); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '/' && c2 == '=') {
                Token t = lex_make_token(lx, TK_SLASH_EQ, "/="); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '%' && c2 == '=') {
                Token t = lex_make_token(lx, TK_PERCENT_EQ, "%="); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == ':' && c2 == ':') {
                Token t = lex_make_token(lx, TK_COLONCOLON, "::"); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '=' && c2 == '>') {
                Token t = lex_make_token(lx, TK_FAT_ARROW, "=>"); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }
            if (c1 == '.' && c2 == '.') {
                if (lx->pos + 2 < lx->src_len && lx->src[lx->pos+2] == '.') {
                    Token t = lex_make_token(lx, TK_DOTDOTDOT, "..."); t.line = start_line; t.col = start_col;
                    lex_add_token(lx, t); lx->pos += 3; lx->col += 3; continue;
                }
                Token t = lex_make_token(lx, TK_DOTDOT, ".."); t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos += 2; lx->col += 2; continue;
            }

            // 单字符
            TokenType single = TK_INVALID;
            const char *str = NULL;
            switch (c1) {
                case '+': single = TK_PLUS;       str = "+"; break;
                case '-': single = TK_MINUS;      str = "-"; break;
                case '*': single = TK_STAR;       str = "*"; break;
                case '/': single = TK_SLASH;      str = "/"; break;
                case '%': single = TK_PERCENT;    str = "%"; break;
                case '=': single = TK_ASSIGN;     str = "="; break;
                case '<': single = TK_LT;         str = "<"; break;
                case '>': single = TK_GT;         str = ">"; break;
                case '!': single = TK_BANG;       str = "!"; break;
                case '&': single = TK_BIT_AND;    str = "&"; break;
                case '|': single = TK_BIT_OR;     str = "|"; break;
                case '^': single = TK_BIT_XOR;    str = "^"; break;
                case '~': single = TK_BIT_NOT;    str = "~"; break;
                case '.': single = TK_DOT;        str = "."; break;
                case ',': single = TK_COMMA;      str = ","; break;
                case ':': single = TK_COLON;      str = ":"; break;
                case ';': single = TK_SEMICOLON;  str = ";"; break;
                case '(': single = TK_LPAREN;     str = "("; break;
                case ')': single = TK_RPAREN;     str = ")"; break;
                case '{': single = TK_LBRACE;     str = "{"; break;
                case '}': single = TK_RBRACE;     str = "}"; break;
                case '[': single = TK_LBRACKET;   str = "["; break;
                case ']': single = TK_RBRACKET;   str = "]"; break;
                case '?': single = TK_QUESTION;   str = "?"; break;
                default:  break;
            }
            if (single != TK_INVALID) {
                Token t = lex_make_token(lx, single, str);
                t.line = start_line; t.col = start_col;
                lex_add_token(lx, t); lx->pos++; lx->col++;
                continue;
            }
        }

        /* ── 8. 无法识别的字符 ── */
        yao_error(PHASE_LEX, start_line, start_col, "无法识别的字符: 0x%02X '%c'", c, (c > 31 && c < 127) ? c : '?');
        lx->pos++; lx->col++;
    }

    /* 添加 EOF */
    Token eof = lex_make_token(lx, TK_EOF, "");
    eof.line = lx->line;
    eof.col  = lx->col;
    lex_add_token(lx, eof);
}

/* ════════════════════════════════════════════════════════════════════
 * AST (抽象语法树)
 * ════════════════════════════════════════════════════════════════════ */

typedef enum {
    AST_PROGRAM,
    /* 声明 */
    AST_FN_DECL,
    AST_VAR_DECL,
    AST_STRUCT_DECL,
    AST_ENUM_DECL,
    AST_TRAIT_DECL,
    AST_IMPL_DECL,
    AST_USE_DECL,
    AST_MOD_DECL,
    AST_TYPE_ALIAS,
    AST_EXTERN_DECL,
    /* 语句 */
    AST_EXPR_STMT,
    AST_BLOCK,
    AST_IF_STMT,
    AST_WHILE_STMT,
    AST_LOOP_STMT,
    AST_FOR_STMT,
    AST_BREAK,
    AST_CONTINUE,
    AST_RETURN,
    AST_ASSIGN,
    AST_MATCH_STMT,
    /* 表达式 */
    AST_INT_LIT,
    AST_FLOAT_LIT,
    AST_STRING_LIT,
    AST_CHAR_LIT,
    AST_BOOL_LIT,
    AST_NIL_LIT,
    AST_IDENT,
    AST_BINOP,
    AST_UNOP,
    AST_CALL,
    AST_MEMBER_ACCESS,
    AST_INDEX,
    AST_ARRAY_LIT,
    AST_INIT_LIST,    // 结构体初始化
    AST_TYPE_REF,     // 类型引用
    AST_CAST,         // 类型转换
    AST_REF_EXPR,     // 引用表达式
    AST_DEREF_EXPR,   // 解引用
    AST_NEW_EXPR,     // 新建表达式
    AST_LAMBDA,       // 闭包/匿名函数
    AST_SIZEOF_EXPR,
    AST_TRY_EXPR,
    AST_THROW_EXPR,
    AST_AWAIT_EXPR,
} AstType;

typedef struct AstNode AstNode;

typedef struct {
    AstNode *items[MAX_AST_CHILDREN];
    int count;
} AstList;

struct AstNode {
    AstType type;
    Token   tok;          // 关联token
    /* 通用子节点 */
    AstNode *child[8];
    AstList children;     // 用于Block等
    /* 字面值 */
    int64_t  int_val;
    double   float_val;
    char     str_val[MAX_TOKEN_LEN];
    /* 运算符 */
    TokenType op;
    /* 名称 */
    char name[MAX_TOKEN_LEN];
    /* 类型注解 */
    char type_name[MAX_TOKEN_LEN];
    /* 参数列表 */
    struct {
        char name[MAX_TOKEN_LEN];
        char type_name[MAX_TOKEN_LEN];
    } params[MAX_FN_PARAMS];
    int param_count;
    /* 返回类型 */
    char ret_type[MAX_TOKEN_LEN];
    /* 可变性/可见性标志 */
    bool is_mut;
    bool is_pub;
    bool is_extern;
    bool is_static;
    /* 结构体成员 */
    struct {
        char name[MAX_TOKEN_LEN];
        char type_name[MAX_TOKEN_LEN];
        bool is_pub;
    } members[MAX_STRUCT_MEMBERS];
    int member_count;
    /* 枚举变体 */
    struct {
        char name[MAX_TOKEN_LEN];
        int64_t value;
    } variants[MAX_ENUM_VARIANTS];
    int variant_count;
    /* match 分支 */
    struct {
        AstNode *pattern;   // 模式
        AstNode *body;      // 分支体
    } match_arms[64];
    int match_arm_count;
    /* 额外 */
    char impl_type[MAX_TOKEN_LEN];    // 实现的目标类型
    char impl_trait[MAX_TOKEN_LEN];   // 实现的特质 (可选)
};

static AstNode *ast_new(AstType t) {
    AstNode *n = (AstNode *)calloc(1, sizeof(AstNode));
    n->type = t;
    return n;
}

static void ast_add_child(AstNode *parent, AstNode *child) {
    if (parent->children.count < MAX_AST_CHILDREN)
        parent->children.items[parent->children.count++] = child;
}

/* ════════════════════════════════════════════════════════════════════
 * 语法分析器 (递归下降)
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    Token *tokens;
    int   pos;
    int   count;
} Parser;

static Token *peek(Parser *p) { return &p->tokens[p->pos]; }
static Token *peek_at(Parser *p, int off) {
    int i = p->pos + off;
    return (i >= 0 && i < p->count) ? &p->tokens[i] : &p->tokens[p->count - 1];
}
static Token *advance(Parser *p) { return &p->tokens[p->pos++]; }
static bool check(Parser *p, TokenType t) { return p->tokens[p->pos].type == t; }
static bool match(Parser *p, TokenType t) {
    if (p->tokens[p->pos].type == t) { p->pos++; return true; }
    return false;
}

static Token *expect(Parser *p, TokenType t, const char *what) {
    if (p->tokens[p->pos].type != t) {
        Token *tk = &p->tokens[p->pos];
        yao_error(PHASE_PARSE, tk->line, tk->col,
            "期望 %s, 但得到 '%s'", what, tk->text[0] ? tk->text : "文件末尾");
        return tk;
    }
    return &p->tokens[p->pos++];
}

/* ── 前向声明 ── */
static AstNode *parse_expr(Parser *p);
static AstNode *parse_stmt(Parser *p);
static AstNode *parse_block(Parser *p);
static AstNode *parse_type_ref(Parser *p);
static AstNode *parse_decl(Parser *p);
static AstNode *parse_or_expr(Parser *p);
static AstNode *parse_if_rest(Parser *p);

/* 解析类型引用 */
static AstNode *parse_type_ref(Parser *p) {
    Token *tk = peek(p);
    AstNode *node = ast_new(AST_TYPE_REF);
    node->tok = *tk;

    /* 类型可以是关键字类型或标识符 */
    switch (tk->type) {
        case TK_TYPE_INT:    strncpy(node->name, "整数", sizeof(node->name)); advance(p); break;
        case TK_TYPE_FLOAT:  strncpy(node->name, "浮点", sizeof(node->name)); advance(p); break;
        case TK_TYPE_BOOL:   strncpy(node->name, "布尔", sizeof(node->name)); advance(p); break;
        case TK_TYPE_STRING: strncpy(node->name, "文本", sizeof(node->name)); advance(p); break;
        case TK_TYPE_CHAR:   strncpy(node->name, "字符", sizeof(node->name)); advance(p); break;
        case TK_TYPE_VOID:   strncpy(node->name, "空类型", sizeof(node->name)); advance(p); break;
        case TK_IDENT:
            strncpy(node->name, tk->text, sizeof(node->name));
            advance(p);
            break;
        default:
            yao_error(PHASE_PARSE, tk->line, tk->col,
                "期望类型名, 但得到 '%s'", tk->text[0] ? tk->text : "文件末尾");
            advance(p);
            strncpy(node->name, "整数", sizeof(node->name));
            break;
    }

    /* 数组类型: 类型[大小] 或 类型[] */
    if (check(p, TK_LBRACKET)) {
        advance(p);
        if (!check(p, TK_RBRACKET)) {
            AstNode *size = parse_expr(p);
            node->child[0] = size;
        }
        expect(p, TK_RBRACKET, "']'");
        strcat(node->name, "[]");
    }

    /* 引用类型: &类型 → 用 引用 类型 语法 */
    return node;
}

/* 判断当前token是否是类型开头 (类型关键字或标识符) */
static bool is_type_start(Parser *p) {
    TokenType t = peek(p)->type;
    return t == TK_TYPE_INT || t == TK_TYPE_FLOAT || t == TK_TYPE_BOOL ||
           t == TK_TYPE_STRING || t == TK_TYPE_CHAR || t == TK_TYPE_VOID ||
           t == TK_IDENT;
}

/* 解析参数列表 — 支持两种语法:
   1) 名称: 类型   (冒号分隔, 类似 Pascal/Rust)
   2) 类型 名称     (类型在前, 类似 C/Go)
*/
static void parse_params(Parser *p, AstNode *fn) {
    expect(p, TK_LPAREN, "'('");
    while (!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
        if (fn->param_count >= MAX_FN_PARAMS) {
            yao_error(PHASE_PARSE, peek(p)->line, peek(p)->col, "参数过多");
            break;
        }
        // 可变 修饰
        if (check(p, TK_KW_MUT)) { advance(p); }

        /* 智能判断参数语法风格:
           如果第一个token是类型关键字(整数/浮点等), 则 -> 类型 名称
           如果第一个token是标识符且第二个是冒号 -> 名称: 类型
           否则尝试 类型 名称 (标识符当类型名)
        */
        Token *first = peek(p);
        Token *second = peek_at(p, 1);

        if (first->type == TK_TYPE_INT || first->type == TK_TYPE_FLOAT ||
            first->type == TK_TYPE_BOOL || first->type == TK_TYPE_STRING ||
            first->type == TK_TYPE_CHAR || first->type == TK_TYPE_VOID) {
            // 风格2: 类型 名称
            AstNode *type = parse_type_ref(p);
            strncpy(fn->params[fn->param_count].type_name, type->name, sizeof(fn->params[0].type_name));
            free(type);

            Token *name = expect(p, TK_IDENT, "参数名");
            strncpy(fn->params[fn->param_count].name, name->text, sizeof(fn->params[0].name));
        } else if (first->type == TK_IDENT && second->type == TK_COLON) {
            // 风格1: 名称: 类型
            Token *name = advance(p);
            strncpy(fn->params[fn->param_count].name, name->text, sizeof(fn->params[0].name));
            advance(p); // 跳过冒号

            AstNode *type = parse_type_ref(p);
            strncpy(fn->params[fn->param_count].type_name, type->name, sizeof(fn->params[0].type_name));
            free(type);
        } else {
            // 尝试: 类型 名称 (用户自定义类型)
            AstNode *type = parse_type_ref(p);
            strncpy(fn->params[fn->param_count].type_name, type->name, sizeof(fn->params[0].type_name));
            free(type);

            Token *name = expect(p, TK_IDENT, "参数名");
            strncpy(fn->params[fn->param_count].name, name->text, sizeof(fn->params[0].name));
        }

        fn->param_count++;

        if (!match(p, TK_COMMA)) break;
    }
    expect(p, TK_RPAREN, "')'");
}

/* 解析函数声明 */
static AstNode *parse_fn_decl(Parser *p, bool is_pub, bool is_extern, bool is_static) {
    Token *fn_kw = expect(p, TK_KW_FN, "'函数'");
    AstNode *fn = ast_new(AST_FN_DECL);
    fn->tok = *fn_kw;
    fn->is_pub = is_pub;
    fn->is_extern = is_extern;
    fn->is_static = is_static;

    Token *name = expect(p, TK_IDENT, "函数名");
    strncpy(fn->name, name->text, sizeof(fn->name));

    parse_params(p, fn);

    /* 返回类型: → 类型 或省略(默认空) */
    if (check(p, TK_ARROW) || check(p, TK_CN_ARROW)) {
        advance(p);
        AstNode *ret = parse_type_ref(p);
        strncpy(fn->ret_type, ret->name, sizeof(fn->ret_type));
        free(ret);
    } else if (check(p, TK_COLON)) {
        // 也支持 函数 name(): 类型
        advance(p);
        AstNode *ret = parse_type_ref(p);
        strncpy(fn->ret_type, ret->name, sizeof(fn->ret_type));
        free(ret);
    } else {
        strncpy(fn->ret_type, "空类型", sizeof(fn->ret_type));
    }

    /* extern 函数没有函数体 */
    if (is_extern) {
        match(p, TK_SEMICOLON);
        return fn;
    }

    /* 函数体 */
    fn->child[0] = parse_block(p);
    return fn;
}

/* 解析变量声明: 变量/令 名称[: 类型] = 表达式 */
static AstNode *parse_var_decl(Parser *p, bool is_pub, bool is_static) {
    Token *kw; // 变量 或 令
    bool is_mut = false;
    if (check(p, TK_KW_VAR)) {
        kw = advance(p);
        is_mut = true;
    } else if (check(p, TK_KW_LET)) {
        kw = advance(p);
    } else if (check(p, TK_KW_CONST)) {
        kw = advance(p);
    } else if (check(p, TK_KW_MUT)) {
        kw = advance(p);
        is_mut = true;
        // 可变 令 的语法: 可变 令 x = ...
        if (check(p, TK_KW_LET)) advance(p);
    } else {
        kw = &p->tokens[p->pos]; // shouldn't happen
    }

    AstNode *vd = ast_new(AST_VAR_DECL);
    vd->tok = *kw;
    vd->is_pub = is_pub;
    vd->is_static = is_static;
    vd->is_mut = is_mut;

    Token *name = expect(p, TK_IDENT, "变量名");
    strncpy(vd->name, name->text, sizeof(vd->name));

    /* 类型注解 (可选) */
    if (match(p, TK_COLON)) {
        AstNode *type = parse_type_ref(p);
        strncpy(vd->type_name, type->name, sizeof(vd->type_name));
        free(type);
    }

    /* 初始化 */
    if (match(p, TK_ASSIGN) || match(p, TK_CN_ASSIGN)) {
        vd->child[0] = parse_expr(p);
    }

    match(p, TK_SEMICOLON);
    return vd;
}

/* 解析结构体 */
static AstNode *parse_struct_decl(Parser *p, bool is_pub) {
    Token *kw = expect(p, TK_KW_STRUCT, "'结构'");
    AstNode *st = ast_new(AST_STRUCT_DECL);
    st->tok = *kw;
    st->is_pub = is_pub;

    Token *name = expect(p, TK_IDENT, "结构体名");
    strncpy(st->name, name->text, sizeof(st->name));

    expect(p, TK_LBRACE, "'{'");
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        if (st->member_count >= MAX_STRUCT_MEMBERS) {
            yao_error(PHASE_PARSE, peek(p)->line, peek(p)->col, "成员过多");
            break;
        }
        bool member_pub = false;
        if (match(p, TK_KW_PUB)) member_pub = true;

        /* 支持 类型 名称 和 名称: 类型 */
        Token *first = peek(p);
        Token *second = peek_at(p, 1);
        char mname_buf[MAX_TOKEN_LEN] = {0};
        char mtype_buf[MAX_TOKEN_LEN] = {0};

        if (first->type == TK_TYPE_INT || first->type == TK_TYPE_FLOAT ||
            first->type == TK_TYPE_BOOL || first->type == TK_TYPE_STRING ||
            first->type == TK_TYPE_CHAR || first->type == TK_TYPE_VOID) {
            // 类型 名称
            AstNode *mtype = parse_type_ref(p);
            strncpy(mtype_buf, mtype->name, sizeof(mtype_buf));
            free(mtype);
            Token *mname = expect(p, TK_IDENT, "成员名");
            strncpy(mname_buf, mname->text, sizeof(mname_buf));
        } else if (first->type == TK_IDENT && second && second->type == TK_COLON) {
            // 名称: 类型
            Token *mname = advance(p);
            strncpy(mname_buf, mname->text, sizeof(mname_buf));
            advance(p); // :
            AstNode *mtype = parse_type_ref(p);
            strncpy(mtype_buf, mtype->name, sizeof(mtype_buf));
            free(mtype);
        } else {
            // 尝试 用户类型 名称
            AstNode *mtype = parse_type_ref(p);
            strncpy(mtype_buf, mtype->name, sizeof(mtype_buf));
            free(mtype);
            Token *mname = expect(p, TK_IDENT, "成员名");
            strncpy(mname_buf, mname->text, sizeof(mname_buf));
        }

        strncpy(st->members[st->member_count].name, mname_buf, sizeof(st->members[0].name));
        strncpy(st->members[st->member_count].type_name, mtype_buf, sizeof(st->members[0].type_name));
        st->members[st->member_count].is_pub = member_pub;
        st->member_count++;

        match(p, TK_COMMA);
        match(p, TK_SEMICOLON);
    }
    expect(p, TK_RBRACE, "'}'");
    return st;
}

/* 解析枚举 */
static AstNode *parse_enum_decl(Parser *p, bool is_pub) {
    Token *kw = expect(p, TK_KW_ENUM, "'枚举'");
    AstNode *en = ast_new(AST_ENUM_DECL);
    en->tok = *kw;
    en->is_pub = is_pub;

    Token *name = expect(p, TK_IDENT, "枚举名");
    strncpy(en->name, name->text, sizeof(en->name));

    expect(p, TK_LBRACE, "'{'");
    int64_t auto_val = 0;
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        if (en->variant_count >= MAX_ENUM_VARIANTS) break;
        Token *vname = expect(p, TK_IDENT, "枚举变体名");
        strncpy(en->variants[en->variant_count].name, vname->text, sizeof(en->variants[0].name));
        if (match(p, TK_ASSIGN) || match(p, TK_CN_ASSIGN)) {
            AstNode *val = parse_expr(p);
            en->variants[en->variant_count].value = val ? val->int_val : auto_val;
            free(val);
        } else {
            en->variants[en->variant_count].value = auto_val;
        }
        auto_val = en->variants[en->variant_count].value + 1;
        en->variant_count++;
        match(p, TK_COMMA);
    }
    expect(p, TK_RBRACE, "'}'");
    return en;
}

/* 解析特质 */
static AstNode *parse_trait_decl(Parser *p, bool is_pub) {
    Token *kw = expect(p, TK_KW_TRAIT, "'特质'");
    AstNode *tr = ast_new(AST_TRAIT_DECL);
    tr->tok = *kw;
    tr->is_pub = is_pub;

    Token *name = expect(p, TK_IDENT, "特质名");
    strncpy(tr->name, name->text, sizeof(tr->name));

    expect(p, TK_LBRACE, "'{'");
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        // 方法签名: 函数 name(params) → 类型;
        bool mpub = match(p, TK_KW_PUB);
        if (check(p, TK_KW_FN)) {
            AstNode *method = parse_fn_decl(p, mpub, false, false);
            ast_add_child(tr, method);
        }
        match(p, TK_SEMICOLON);
    }
    expect(p, TK_RBRACE, "'}'");
    return tr;
}

/* 解析实现 */
static AstNode *parse_impl_decl(Parser *p) {
    Token *kw = expect(p, TK_KW_IMPL, "'实现'");
    AstNode *im = ast_new(AST_IMPL_DECL);
    im->tok = *kw;

    // 可选: 实现 特质名 为 类型名 { ... }
    AstNode *target = parse_type_ref(p);
    strncpy(im->impl_type, target->name, sizeof(im->impl_type));
    free(target);

    if (match(p, TK_KW_AS)) {
        // 实现 特质 为 类型
        AstNode *real_type = parse_type_ref(p);
        strncpy(im->impl_trait, im->impl_type, sizeof(im->impl_trait));
        strncpy(im->impl_type, real_type->name, sizeof(im->impl_type));
        free(real_type);
    }

    expect(p, TK_LBRACE, "'{'");
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        bool mpub = match(p, TK_KW_PUB);
        if (check(p, TK_KW_FN)) {
            AstNode *method = parse_fn_decl(p, mpub, false, false);
            ast_add_child(im, method);
        }
        match(p, TK_SEMICOLON);
    }
    expect(p, TK_RBRACE, "'}'");
    return im;
}

/* ── 表达式解析 (优先级递降) ── */

static AstNode *parse_primary(Parser *p) {
    Token *tk = peek(p);

    switch (tk->type) {
        case TK_INT: {
            AstNode *n = ast_new(AST_INT_LIT);
            n->tok = *tk; n->int_val = tk->int_val;
            strncpy(n->str_val, tk->text, sizeof(n->str_val));
            advance(p); return n;
        }
        case TK_FLOAT: {
            AstNode *n = ast_new(AST_FLOAT_LIT);
            n->tok = *tk; n->float_val = tk->float_val;
            strncpy(n->str_val, tk->text, sizeof(n->str_val));
            advance(p); return n;
        }
        case TK_STRING: {
            AstNode *n = ast_new(AST_STRING_LIT);
            n->tok = *tk; strncpy(n->str_val, tk->text, sizeof(n->str_val));
            advance(p); return n;
        }
        case TK_CHAR: {
            AstNode *n = ast_new(AST_CHAR_LIT);
            n->tok = *tk; n->int_val = tk->int_val;
            strncpy(n->str_val, tk->text, sizeof(n->str_val));
            advance(p); return n;
        }
        case TK_BOOL_TRUE: {
            AstNode *n = ast_new(AST_BOOL_LIT);
            n->tok = *tk; n->int_val = 1; strncpy(n->str_val, "真", sizeof(n->str_val));
            advance(p); return n;
        }
        case TK_BOOL_FALSE: {
            AstNode *n = ast_new(AST_BOOL_LIT);
            n->tok = *tk; n->int_val = 0; strncpy(n->str_val, "假", sizeof(n->str_val));
            advance(p); return n;
        }
        case TK_KWNIL: {
            AstNode *n = ast_new(AST_NIL_LIT);
            n->tok = *tk; strncpy(n->str_val, "空", sizeof(n->str_val));
            advance(p); return n;
        }
        case TK_IDENT: {
            AstNode *n = ast_new(AST_IDENT);
            n->tok = *tk; strncpy(n->name, tk->text, sizeof(n->name));
            advance(p);
            /* 后续可跟 -> 成员访问, ( -> 调用, [ -> 索引 */
            while (true) {
                if (check(p, TK_DOT) || check(p, TK_CN_MEMBER)) {
                    advance(p);
                    Token *member = expect(p, TK_IDENT, "成员名");
                    AstNode *ma = ast_new(AST_MEMBER_ACCESS);
                    ma->tok = *member;
                    ma->child[0] = n;
                    strncpy(ma->name, member->text, sizeof(ma->name));
                    n = ma;
                } else if (check(p, TK_LPAREN)) {
                    advance(p);
                    AstNode *call = ast_new(AST_CALL);
                    call->tok = n->tok;
                    call->child[0] = n;
                    while (!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
                        AstNode *arg = parse_expr(p);
                        ast_add_child(call, arg);
                        if (!match(p, TK_COMMA)) break;
                    }
                    expect(p, TK_RPAREN, "')'");
                    n = call;
                } else if (check(p, TK_LBRACKET)) {
                    advance(p);
                    AstNode *idx = ast_new(AST_INDEX);
                    idx->tok = n->tok;
                    idx->child[0] = n;
                    if (!check(p, TK_RBRACKET)) {
                        idx->child[1] = parse_expr(p);
                    }
                    expect(p, TK_RBRACKET, "']'");
                    n = idx;
                } else {
                    break;
                }
            }
            return n;
        }
        case TK_KW_SELF: {
            AstNode *n = ast_new(AST_IDENT);
            n->tok = *tk; strncpy(n->name, "自身", sizeof(n->name));
            advance(p); return n;
        }
        case TK_LPAREN: {
            advance(p);
            AstNode *e = parse_expr(p);
            expect(p, TK_RPAREN, "')'");
            return e;
        }
        case TK_LBRACKET: {
            advance(p);
            AstNode *arr = ast_new(AST_ARRAY_LIT);
            arr->tok = *tk;
            while (!check(p, TK_RBRACKET) && !check(p, TK_EOF)) {
                AstNode *elem = parse_expr(p);
                ast_add_child(arr, elem);
                if (!match(p, TK_COMMA)) break;
            }
            expect(p, TK_RBRACKET, "']'");
            return arr;
        }
        case TK_KW_NEW: {
            advance(p);
            Token *tname = expect(p, TK_IDENT, "类型名");
            AstNode *ne = ast_new(AST_NEW_EXPR);
            ne->tok = *tname; strncpy(ne->name, tname->text, sizeof(ne->name));
            if (match(p, TK_LBRACE)) {
                while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
                    Token *fname = expect(p, TK_IDENT, "字段名");
                    expect(p, TK_ASSIGN, "'='");
                    AstNode *fval = parse_expr(p);
                    /* 用 match_arms 存储 field init: pattern=字段名, body=value */
                    AstNode *field_node = ast_new(AST_EXPR_STMT);
                    strncpy(field_node->name, fname->text, sizeof(field_node->name));
                    field_node->child[0] = fval;
                    ast_add_child(ne, field_node);
                    match(p, TK_COMMA);
                }
                expect(p, TK_RBRACE, "'}'");
            } else if (match(p, TK_LPAREN)) {
                // new Type(args)
                while (!check(p, TK_RPAREN) && !check(p, TK_EOF)) {
                    AstNode *arg = parse_expr(p);
                    ast_add_child(ne, arg);
                    if (!match(p, TK_COMMA)) break;
                }
                expect(p, TK_RPAREN, "')'");
            }
            return ne;
        }
        case TK_MINUS: case TK_BANG: case TK_BIT_NOT: case TK_CN_NOT: {
            advance(p);
            AstNode *u = ast_new(AST_UNOP);
            u->tok = *tk; u->op = tk->type;
            u->child[0] = parse_primary(p);
            return u;
        }
        case TK_KW_REF: case TK_KW_BORROW: {
            advance(p);
            AstNode *r = ast_new(AST_REF_EXPR);
            r->tok = *tk;
            r->child[0] = parse_primary(p);
            return r;
        }
        case TK_KW_SIZEOF: {
            advance(p);
            expect(p, TK_LPAREN, "'('");
            AstNode *s = ast_new(AST_SIZEOF_EXPR);
            s->child[0] = parse_type_ref(p);
            expect(p, TK_RPAREN, "')'");
            return s;
        }
        case TK_KW_TRY: {
            advance(p);
            AstNode *t = ast_new(AST_TRY_EXPR);
            t->child[0] = parse_primary(p);
            return t;
        }
        case TK_KW_AWAIT: {
            advance(p);
            AstNode *a = ast_new(AST_AWAIT_EXPR);
            a->child[0] = parse_primary(p);
            return a;
        }
        case TK_KW_THROW: {
            advance(p);
            AstNode *t = ast_new(AST_THROW_EXPR);
            t->child[0] = parse_expr(p);
            return t;
        }
        case TK_STAR: {
            // *ptr 解引用
            advance(p);
            AstNode *d = ast_new(AST_DEREF_EXPR);
            d->child[0] = parse_primary(p);
            return d;
        }
        default:
            yao_error(PHASE_PARSE, tk->line, tk->col,
                "意外的标记 '%s' (期望表达式)", tk->text[0] ? tk->text : "文件末尾");
            advance(p);
            return ast_new(AST_NIL_LIT);
    }
}

/* 乘除模 */
static AstNode *parse_mul_expr(Parser *p) {
    AstNode *left = parse_primary(p);
    while (true) {
        Token *tk = peek(p);
        TokenType op = tk->type;
        if (op == TK_STAR || op == TK_SLASH || op == TK_PERCENT ||
            op == TK_CN_STAR || op == TK_CN_SLASH) {
            advance(p);
            AstNode *right = parse_primary(p);
            AstNode *b = ast_new(AST_BINOP);
            b->tok = *tk; b->op = op;
            b->child[0] = left; b->child[1] = right;
            left = b;
        } else break;
    }
    return left;
}

/* 加减 */
static AstNode *parse_add_expr(Parser *p) {
    AstNode *left = parse_mul_expr(p);
    while (true) {
        Token *tk = peek(p);
        TokenType op = tk->type;
        if (op == TK_PLUS || op == TK_MINUS || op == TK_CN_PLUS || op == TK_CN_MINUS) {
            advance(p);
            AstNode *right = parse_mul_expr(p);
            AstNode *b = ast_new(AST_BINOP);
            b->tok = *tk; b->op = op;
            b->child[0] = left; b->child[1] = right;
            left = b;
        } else break;
    }
    return left;
}

/* 比较 */
static AstNode *parse_cmp_expr(Parser *p) {
    AstNode *left = parse_add_expr(p);
    while (true) {
        Token *tk = peek(p);
        TokenType op = tk->type;
        if (op == TK_LT || op == TK_LE || op == TK_GT || op == TK_GE ||
            op == TK_CN_LT || op == TK_CN_LE || op == TK_CN_GT || op == TK_CN_GE) {
            advance(p);
            AstNode *right = parse_add_expr(p);
            AstNode *b = ast_new(AST_BINOP);
            b->tok = *tk; b->op = op;
            b->child[0] = left; b->child[1] = right;
            left = b;
        } else break;
    }
    return left;
}

/* 相等 */
static AstNode *parse_eq_expr(Parser *p) {
    AstNode *left = parse_cmp_expr(p);
    while (true) {
        Token *tk = peek(p);
        TokenType op = tk->type;
        if (op == TK_EQ_EQ || op == TK_BANG_EQ || op == TK_CN_EQ || op == TK_CN_NEQ) {
            advance(p);
            AstNode *right = parse_cmp_expr(p);
            AstNode *b = ast_new(AST_BINOP);
            b->tok = *tk; b->op = op;
            b->child[0] = left; b->child[1] = right;
            left = b;
        } else break;
    }
    return left;
}

/* 位运算 */
static AstNode *parse_bit_expr(Parser *p) {
    AstNode *left = parse_eq_expr(p);
    while (true) {
        Token *tk = peek(p);
        TokenType op = tk->type;
        if (op == TK_BIT_AND || op == TK_BIT_OR || op == TK_BIT_XOR || op == TK_SHL || op == TK_SHR) {
            advance(p);
            AstNode *right = parse_eq_expr(p);
            AstNode *b = ast_new(AST_BINOP);
            b->tok = *tk; b->op = op;
            b->child[0] = left; b->child[1] = right;
            left = b;
        } else break;
    }
    return left;
}

/* 逻辑与 */
static AstNode *parse_and_expr(Parser *p) {
    AstNode *left = parse_bit_expr(p);
    while (true) {
        Token *tk = peek(p);
        if (tk->type == TK_AND || tk->type == TK_CN_AND) {
            advance(p);
            AstNode *right = parse_bit_expr(p);
            AstNode *b = ast_new(AST_BINOP);
            b->tok = *tk; b->op = tk->type;
            b->child[0] = left; b->child[1] = right;
            left = b;
        } else break;
    }
    return left;
}

/* 逻辑或 */
static AstNode *parse_or_expr(Parser *p) {
    AstNode *left = parse_and_expr(p);
    while (true) {
        Token *tk = peek(p);
        if (tk->type == TK_OR || tk->type == TK_CN_OR) {
            advance(p);
            AstNode *right = parse_and_expr(p);
            AstNode *b = ast_new(AST_BINOP);
            b->tok = *tk; b->op = tk->type;
            b->child[0] = left; b->child[1] = right;
            left = b;
        } else break;
    }
    return left;
}

/* 赋值 */
static AstNode *parse_assign(Parser *p) {
    AstNode *left = parse_or_expr(p);
    Token *tk = peek(p);
    TokenType op = tk->type;
    if (op == TK_ASSIGN || op == TK_CN_ASSIGN || op == TK_PLUS_EQ || op == TK_MINUS_EQ ||
        op == TK_STAR_EQ || op == TK_SLASH_EQ || op == TK_PERCENT_EQ) {
        advance(p);
        AstNode *right = parse_assign(p);
        AstNode *a = ast_new(AST_ASSIGN);
        a->tok = *tk; a->op = op;
        a->child[0] = left; a->child[1] = right;
        return a;
    }
    /* 类型转换: 表达式 作为 类型 */
    if (op == TK_KW_AS) {
        advance(p);
        AstNode *type = parse_type_ref(p);
        AstNode *c = ast_new(AST_CAST);
        c->tok = *tk;
        c->child[0] = left;
        strncpy(c->type_name, type->name, sizeof(c->type_name));
        free(type);
        return c;
    }
    return left;
}

static AstNode *parse_expr(Parser *p) {
    return parse_assign(p);
}

/* ── 语句 ── */

static AstNode *parse_block(Parser *p) {
    Token *lb = expect(p, TK_LBRACE, "'{'");
    AstNode *blk = ast_new(AST_BLOCK);
    blk->tok = *lb;
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        AstNode *s = parse_stmt(p);
        if (s) ast_add_child(blk, s);
    }
    expect(p, TK_RBRACE, "'}'");
    return blk;
}

static AstNode *parse_if_rest(Parser *p) {
    /* 已经消耗了否则若 token, 直接解析条件和分支 */
    AstNode *node = ast_new(AST_IF_STMT);
    Token saved = *peek(p);
    node->tok = saved;

    node->child[0] = parse_expr(p);  // 条件
    node->child[1] = parse_block(p); // then

    if (check(p, TK_KW_ELSE_IF)) {
        advance(p);
        node->child[2] = parse_if_rest(p);
    } else if (check(p, TK_KW_ELSE)) {
        advance(p);
        if (check(p, TK_LBRACE)) {
            node->child[2] = parse_block(p);
        } else if (check(p, TK_KW_IF)) {
            advance(p);
            node->child[2] = parse_if_rest(p);
        }
    }
    return node;
}

static AstNode *parse_if(Parser *p) {
    Token *kw = expect(p, TK_KW_IF, "'若'");
    AstNode *node = ast_new(AST_IF_STMT);
    node->tok = *kw;

    node->child[0] = parse_expr(p);  // 条件
    node->child[1] = parse_block(p); // then

    if (check(p, TK_KW_ELSE_IF)) {
        advance(p);
        node->child[2] = parse_if_rest(p);
    } else if (check(p, TK_KW_ELSE)) {
        advance(p);
        if (check(p, TK_LBRACE)) {
            node->child[2] = parse_block(p);
        } else if (check(p, TK_KW_IF)) {
            advance(p);
            node->child[2] = parse_if_rest(p);
        }
    }
    return node;
}

static AstNode *parse_while(Parser *p) {
    Token *kw = expect(p, TK_KW_WHILE, "'当'");
    AstNode *node = ast_new(AST_WHILE_STMT);
    node->tok = *kw;
    node->child[0] = parse_expr(p);
    node->child[1] = parse_block(p);
    return node;
}

static AstNode *parse_loop(Parser *p) {
    Token *kw = expect(p, TK_KW_LOOP, "'循环'");
    AstNode *node = ast_new(AST_LOOP_STMT);
    node->tok = *kw;
    node->child[0] = parse_block(p);
    return node;
}

static AstNode *parse_for(Parser *p) {
    Token *kw = expect(p, TK_KW_FOR, "'对于'");
    AstNode *node = ast_new(AST_FOR_STMT);
    node->tok = *kw;

    Token *var = expect(p, TK_IDENT, "循环变量名");
    strncpy(node->name, var->text, sizeof(node->name));

    expect(p, TK_KW_IN, "'在'");

    /* 范围: 特殊解析 a..b 和 a..=b */
    {
        AstNode *start_expr = parse_primary(p);
        Token *tk = peek(p);
        if (tk->type == TK_DOTDOT || tk->type == TK_DOTDOTDOT) {
            advance(p);
            AstNode *end_expr = parse_primary(p);
            AstNode *range = ast_new(AST_BINOP);
            range->tok = *tk;
            range->op = tk->type;
            range->child[0] = start_expr;
            range->child[1] = end_expr;
            node->child[0] = range;
        } else {
            // 不是范围, 就是一个表达式
            // 回退一下? 不对, start_expr 已经消耗了一个token
            // 把 start_expr 用作变量? 不合理。直接当 range
            // 简单处理: 返回 start_expr 作为遍历对象
            node->child[0] = start_expr;
        }
    }

    node->child[1] = parse_block(p);
    return node;
}

static AstNode *parse_match(Parser *p) {
    Token *kw = expect(p, TK_KW_MATCH, "'匹配'");
    AstNode *node = ast_new(AST_MATCH_STMT);
    node->tok = *kw;

    node->child[0] = parse_expr(p); // 匹配目标
    expect(p, TK_LBRACE, "'{'");

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        if (node->match_arm_count >= 64) {
            yao_error(PHASE_PARSE, peek(p)->line, peek(p)->col, "匹配分支过多");
            break;
        }
        // 模式可以是: 字面值、标识符、_ (通配, 用标识符代替)
        AstNode *pattern;
        Token *tk = peek(p);
        if (check(p, TK_IDENT) && strcmp(tk->text, "_") == 0) {
            advance(p);
            pattern = ast_new(AST_IDENT);
            strncpy(pattern->name, "_", sizeof(pattern->name));
        } else {
            pattern = parse_expr(p);
        }

        expect(p, TK_FAT_ARROW, "'=>'");
        AstNode *body = parse_expr(p);
        match(p, TK_COMMA);
        match(p, TK_SEMICOLON);

        node->match_arms[node->match_arm_count].pattern = pattern;
        node->match_arms[node->match_arm_count].body = body;
        node->match_arm_count++;
    }
    expect(p, TK_RBRACE, "'}'");
    return node;
}

static AstNode *parse_stmt(Parser *p) {
    Token *tk = peek(p);

    switch (tk->type) {
        case TK_LBRACE: return parse_block(p);
        case TK_KW_IF: return parse_if(p);
        case TK_KW_WHILE: return parse_while(p);
        case TK_KW_LOOP: return parse_loop(p);
        case TK_KW_FOR: return parse_for(p);
        case TK_KW_BREAK:
            advance(p); match(p, TK_SEMICOLON);
            { AstNode *n = ast_new(AST_BREAK); n->tok = *tk; return n; }
        case TK_KW_CONTINUE:
            advance(p); match(p, TK_SEMICOLON);
            { AstNode *n = ast_new(AST_CONTINUE); n->tok = *tk; return n; }
        case TK_KW_RETURN: {
            advance(p);
            AstNode *n = ast_new(AST_RETURN);
            n->tok = *tk;
            if (!check(p, TK_SEMICOLON) && !check(p, TK_RBRACE) && !check(p, TK_EOF)) {
                n->child[0] = parse_expr(p);
            }
            match(p, TK_SEMICOLON);
            return n;
        }
        case TK_KW_MATCH: return parse_match(p);
        default:
            break;
    }

    /* 尝试声明 */
    if (tk->type == TK_KW_FN || tk->type == TK_KW_VAR || tk->type == TK_KW_LET ||
        tk->type == TK_KW_CONST || tk->type == TK_KW_MUT || tk->type == TK_KW_STRUCT ||
        tk->type == TK_KW_ENUM || tk->type == TK_KW_TRAIT || tk->type == TK_KW_IMPL ||
        tk->type == TK_KW_USE || tk->type == TK_KW_MOD || tk->type == TK_KW_TYPE ||
        tk->type == TK_KW_EXTERN || tk->type == TK_KW_PUB || tk->type == TK_KW_PRIV) {
        return parse_decl(p);
    }

    /* 表达式语句 */
    AstNode *e = parse_expr(p);
    match(p, TK_SEMICOLON);
    AstNode *es = ast_new(AST_EXPR_STMT);
    es->tok = e->tok;
    es->child[0] = e;
    return es;
}

/* ── 声明 ── */
static AstNode *parse_decl(Parser *p) {
    bool is_pub = false;
    bool is_static = false;

    /* 可见性修饰可以叠加 */
    while (true) {
        if (check(p, TK_KW_PUB)) { is_pub = true; advance(p); continue; }
        if (check(p, TK_KW_PRIV)) { advance(p); continue; }
        if (check(p, TK_KW_STATIC)) { is_static = true; advance(p); continue; }
        break;
    }

    Token *tk = peek(p);

    switch (tk->type) {
        case TK_KW_FN:     return parse_fn_decl(p, is_pub, false, is_static);
        case TK_KW_EXTERN: {
            advance(p);
            // extern 函数 ...
            if (check(p, TK_KW_FN)) {
                return parse_fn_decl(p, is_pub, true, is_static);
            }
            yao_error(PHASE_PARSE, tk->line, tk->col, "外部声明仅支持函数");
            return ast_new(AST_EXTERN_DECL);
        }
        case TK_KW_VAR:
        case TK_KW_LET:
        case TK_KW_CONST:
        case TK_KW_MUT:
            return parse_var_decl(p, is_pub, is_static);
        case TK_KW_STRUCT:  return parse_struct_decl(p, is_pub);
        case TK_KW_ENUM:    return parse_enum_decl(p, is_pub);
        case TK_KW_TRAIT:   return parse_trait_decl(p, is_pub);
        case TK_KW_IMPL:    return parse_impl_decl(p);
        case TK_KW_USE: {
            advance(p);
            AstNode *u = ast_new(AST_USE_DECL);
            u->tok = *tk;
            Token *mod = expect(p, TK_IDENT, "模块名");
            strncpy(u->name, mod->text, sizeof(u->name));
            // 可能有 :: 子路径
            while (match(p, TK_COLONCOLON)) {
                Token *sub = expect(p, TK_IDENT, "子模块名");
                strncat(u->name, "::", sizeof(u->name) - strlen(u->name) - 1);
                strncat(u->name, sub->text, sizeof(u->name) - strlen(u->name) - 1);
            }
            match(p, TK_SEMICOLON);
            return u;
        }
        case TK_KW_MOD: {
            advance(p);
            AstNode *m = ast_new(AST_MOD_DECL);
            m->tok = *tk;
            Token *name = expect(p, TK_IDENT, "模块名");
            strncpy(m->name, name->text, sizeof(m->name));
            if (check(p, TK_LBRACE)) {
                m->child[0] = parse_block(p);
            }
            return m;
        }
        case TK_KW_TYPE: {
            advance(p);
            AstNode *ta = ast_new(AST_TYPE_ALIAS);
            ta->tok = *tk;
            Token *name = expect(p, TK_IDENT, "类型别名");
            strncpy(ta->name, name->text, sizeof(ta->name));
            expect(p, TK_ASSIGN, "'='");
            AstNode *real = parse_type_ref(p);
            strncpy(ta->type_name, real->name, sizeof(ta->type_name));
            free(real);
            match(p, TK_SEMICOLON);
            return ta;
        }
        default:
            yao_error(PHASE_PARSE, tk->line, tk->col,
                "意外的标记 '%s' (期望声明)", tk->text[0] ? tk->text : "文件末尾");
            advance(p);
            return ast_new(AST_EXPR_STMT);
    }
}

/* parse program — 顶层允许声明和语句混排 */
static AstNode *parse_program(Token *tokens, int count) {
    Parser p = { tokens, 0, count };
    AstNode *prog = ast_new(AST_PROGRAM);
    while (!check(&p, TK_EOF)) {
        Token *tk = peek(&p);
        /* 声明类: 函数/变量/令/常量/结构/枚举/特质/实现/引入/模块/类型/外部/公开/私有/可变 */
        if (tk->type == TK_KW_FN || tk->type == TK_KW_VAR || tk->type == TK_KW_LET ||
            tk->type == TK_KW_CONST || tk->type == TK_KW_MUT || tk->type == TK_KW_STRUCT ||
            tk->type == TK_KW_ENUM || tk->type == TK_KW_TRAIT || tk->type == TK_KW_IMPL ||
            tk->type == TK_KW_USE || tk->type == TK_KW_MOD || tk->type == TK_KW_TYPE ||
            tk->type == TK_KW_EXTERN || tk->type == TK_KW_PUB || tk->type == TK_KW_PRIV) {
            AstNode *d = parse_decl(&p);
            if (d) ast_add_child(prog, d);
        } else {
            /* 语句 (表达式、控制流等) — 作为顶层语句, 包进匿名 main */
            AstNode *s = parse_stmt(&p);
            if (s) ast_add_child(prog, s);
        }
    }
    return prog;
}
/* ════════════════════════════════════════════════════════════════════
 * 曜语 (YaoLang) 编译器 —— 第二部分: 语义分析 + 代码生成 + 驱动
 * 需与 yaolang.c 第一部分合并编译
 * ════════════════════════════════════════════════════════════════════ */

/* ──────────────────────────── 符号表 ──────────────────────────── */

typedef struct {
    char name[MAX_TOKEN_LEN];   // C合法标识符名
    char yao_name[MAX_TOKEN_LEN]; // 原始中文名
    char type_name[MAX_TOKEN_LEN]; // 类型名
    bool is_const;
    bool is_fn;
    bool is_param;
    int  scope_level;
} SymbolEntry;

typedef struct {
    SymbolEntry symbols[MAX_SYMBOLS];
    int symbol_count;
    int scope_level;
} SymbolTable;

static SymbolTable g_symtab;

static void symtab_enter_scope(void) { g_symtab.scope_level++; }
static void symtab_exit_scope(void) { 
    // 移除当前作用域的符号
    for (int i = g_symtab.symbol_count - 1; i >= 0; i--) {
        if (g_symtab.symbols[i].scope_level == g_symtab.scope_level) {
            // 简单移除: 减少计数
            g_symtab.symbol_count = i;
            break;
        }
    }
    g_symtab.scope_level--;
}

static void symtab_add(const char *name, const char *type_name, bool is_const, bool is_fn) {
    if (g_symtab.symbol_count >= MAX_SYMBOLS) return;
    SymbolEntry *e = &g_symtab.symbols[g_symtab.symbol_count++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    strncpy(e->yao_name, name, sizeof(e->yao_name) - 1);
    strncpy(e->type_name, type_name, sizeof(e->type_name) - 1);
    e->is_const = is_const;
    e->is_fn = is_fn;
    e->scope_level = g_symtab.scope_level;
}

/* Find symbol by yao_name (original Chinese name) */
static SymbolEntry *symtab_lookup_yao(const char *yao_name) {
    for (int i = g_symtab.symbol_count - 1; i >= 0; i--) {
        if (strcmp(g_symtab.symbols[i].yao_name, yao_name) == 0) {
            return &g_symtab.symbols[i];
        }
    }
    return NULL;
}

/* Find enum variant C name by yao_name */
static const char *enum_variant_cname(const char *yao_name) {
    SymbolEntry *se = symtab_lookup_yao(yao_name);
    if (se && se->name[0]) return se->name;
    return NULL;
}
static SymbolEntry *symtab_lookup(const char *name) {
    for (int i = g_symtab.symbol_count - 1; i >= 0; i--) {
        if (strcmp(g_symtab.symbols[i].name, name) == 0) {
            return &g_symtab.symbols[i];
        }
    }
    return NULL;
}

/* Register with separate C name and yao name */
static void symtab_add_mapped(const char *c_name, const char *yao_name, const char *type_name, bool is_const, bool is_fn) {
    if (g_symtab.symbol_count >= MAX_SYMBOLS) return;
    SymbolEntry *e = &g_symtab.symbols[g_symtab.symbol_count++];
    strncpy(e->name, c_name, sizeof(e->name) - 1);
    strncpy(e->yao_name, yao_name, sizeof(e->yao_name) - 1);
    strncpy(e->type_name, type_name, sizeof(e->type_name) - 1);
    e->is_const = is_const;
    e->is_fn = is_fn;
}

/* ──────────────────────────── 类型注册表 ──────────────────────────── */
typedef struct {
    char name[MAX_TOKEN_LEN];
    char c_type[64];    // 对应C类型
    bool is_struct;
    bool is_enum;
    int  size;
} TypeEntry;

static TypeEntry g_types[MAX_TYPES];
static int g_type_count = 0;

static void types_init(void) {
    // 内建类型
    const char *builtin[][2] = {
        {"整数",   "long"},
        {"浮点",   "double"},
        {"布尔",   "int"},
        {"文本",   "char*"},  // 文本就是 C 的 char*
        {"字符",   "char"},
        {"空类型", "void"},
    };
    for (int i = 0; i < 6; i++) {
        strncpy(g_types[i].name, builtin[i][0], sizeof(g_types[0].name) - 1);
        strncpy(g_types[i].c_type, builtin[i][1], sizeof(g_types[0].c_type) - 1);
    }
    g_type_count = 6;
}

static TypeEntry *types_find(const char *name) {
    for (int i = 0; i < g_type_count; i++) {
        if (strcmp(g_types[i].name, name) == 0) return &g_types[i];
    }
    return NULL;
}

static void types_add(const char *name, const char *c_type, bool is_struct, int size) {
    if (g_type_count >= MAX_TYPES) return;
    TypeEntry *t = &g_types[g_type_count++];
    strncpy(t->name, name, sizeof(t->name) - 1);
    strncpy(t->c_type, c_type, sizeof(t->c_type) - 1);
    t->is_struct = is_struct;
    t->size = size;
}

/* 中文类型名 → C 类型名 */
static const char *yao_type_to_c(const char *yao_type) {
    // 内建类型
    if (strcmp(yao_type, "整数") == 0)   return "long";
    if (strcmp(yao_type, "浮点") == 0)   return "double";
    if (strcmp(yao_type, "布尔") == 0)   return "int";
    if (strcmp(yao_type, "文本") == 0)   return "char*";
    if (strcmp(yao_type, "字符") == 0)   return "char";
    if (strcmp(yao_type, "空类型") == 0) return "void";

    // 数组类型
    size_t len = strlen(yao_type);
    if (len > 2 && yao_type[len-2] == '[' && yao_type[len-1] == ']') {
        // 简化: 用指针表示
        static char buf[256];
        char base[256];
        strncpy(base, yao_type, len - 2);
        base[len - 2] = '\0';
        snprintf(buf, sizeof(buf), "%s*", yao_type_to_c(base));
        return buf;
    }

    // 用户定义类型
    TypeEntry *t = types_find(yao_type);
    if (t) return t->c_type;

    // 默认: 结构体指针
    return yao_type;
}

/* 标识符 → C 合法标识符 (中文字符转为 _XXXX 编码) */
static void ident_to_c(const char *yao, char *out, size_t out_sz) {
    size_t oi = 0;
    for (size_t i = 0; yao[i] && oi < out_sz - 8; ) {
        unsigned char c = (unsigned char)yao[i];
        if (c < 0x80) {
            // ASCII 直接保留
            out[oi++] = yao[i++];
        } else {
            // UTF-8 多字节 → 转为 _hhhhhh 形式
            int len;
            int32_t cp;
            len = utf8_decode(&yao[i], &cp);
            if (len <= 0) { i++; continue; }
            oi += snprintf(out + oi, out_sz - oi, "_%04x", (unsigned)cp);
            i += len;
        }
    }
    out[oi] = '\0';
    // 确保不以数字开头
    if (out[0] >= '0' && out[0] <= '9') {
        memmove(out + 1, out, strlen(out) + 1);
        out[0] = '_';
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  语义分析 (符号收集 + 类型检查)
 * ════════════════════════════════════════════════════════════════════ */

static void sema_analyze(AstNode *root) {
    if (root->type != AST_PROGRAM) return;

    /* 第一遍: 收集所有顶层声明 */
    for (int i = 0; i < root->children.count; i++) {
        AstNode *decl = root->children.items[i];
        switch (decl->type) {
            case AST_FN_DECL: {
                char cname[MAX_TOKEN_LEN];
                ident_to_c(decl->name, cname, sizeof(cname));
                symtab_add(cname, decl->ret_type, false, true);
                break;
            }
            case AST_STRUCT_DECL: {
                /* 注册结构体类型 */
                char cname[MAX_TOKEN_LEN];
                ident_to_c(decl->name, cname, sizeof(cname));
                types_add(decl->name, cname, true, 0);
                break;
            }
            case AST_ENUM_DECL: {
                char cname[MAX_TOKEN_LEN];
                ident_to_c(decl->name, cname, sizeof(cname));
                types_add(decl->name, "int", false, sizeof(int));
                // 注册枚举变体到符号表: 用原始中文名作为键, C合法全名作为值
                for (int vi = 0; vi < decl->variant_count; vi++) {
                    char vname[MAX_TOKEN_LEN];
                    char full_name[MAX_TOKEN_LEN];
                    ident_to_c(decl->variants[vi].name, vname, sizeof(vname));
                    snprintf(full_name, sizeof(full_name), "%s_%s", cname, vname);
                    symtab_add(full_name, decl->name, true, false);
                    /* 注册变体原始中文名 -> 映射到完整C名 */
                    symtab_add_mapped(full_name, decl->variants[vi].name, decl->name, true, false);
                }
                break;
            }
            case AST_VAR_DECL: {
                char cname[MAX_TOKEN_LEN];
                ident_to_c(decl->name, cname, sizeof(cname));
                symtab_add(cname, decl->type_name[0] ? decl->type_name : "整数", !decl->is_mut, false);
                break;
            }
            case AST_TYPE_ALIAS: {
                /* 类型别名: 在C层用 typedef */
                types_add(decl->name, yao_type_to_c(decl->type_name), false, 0);
                break;
            }
            default: break;
        }
    }

    /* 第二遍: 检查函数体 */
    for (int i = 0; i < root->children.count; i++) {
        AstNode *decl = root->children.items[i];
        if (decl->type == AST_FN_DECL && !decl->is_extern) {
            symtab_enter_scope();
            // 添加参数
            for (int j = 0; j < decl->param_count; j++) {
                char cname[MAX_TOKEN_LEN];
                ident_to_c(decl->params[j].name, cname, sizeof(cname));
                symtab_add(cname, decl->params[j].type_name, false, false);
            }
            // TODO: 深入检查函数体语句 (简化版暂略)
            symtab_exit_scope();
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  代码生成器 (AST → C代码)
 * ════════════════════════════════════════════════════════════════════ */

static StrBuf g_cgen;      // 生成的C代码
static int g_label_counter = 0;
static int g_tmp_counter = 0;
static int g_depth = 0;
static char g_brk_label[128][32];
static int g_brk_top = 0;
static char g_cont_label[128][32];
static int g_cont_top = 0;
static void push_loop(const char* b, const char* c2) {
    snprintf(g_brk_label[g_brk_top++], 32, "%s", b);
    snprintf(g_cont_label[g_cont_top++], 32, "%s", c2);
}
static void pop_loop(void) {
    if (g_brk_top > 0) g_brk_top--;
    if (g_cont_top > 0) g_cont_top--;
}

static char g_label_bufs[128][32];
static int g_label_idx = 0;
static char *new_label(void) {
    int idx = g_label_idx++ % 128;
    snprintf(g_label_bufs[idx], sizeof(g_label_bufs[idx]), "L%d", g_label_counter++);
    return g_label_bufs[idx];
}

static char g_tmp_bufs[128][32];
static int g_tmp_idx = 0;
static char *new_tmp(void) {
    int idx = g_tmp_idx++ % 128;
    snprintf(g_tmp_bufs[idx], sizeof(g_tmp_bufs[idx]), "_t%d", g_tmp_counter++);
    return g_tmp_bufs[idx];
}

/* 生成字符串字面值的C转义 */
static void cgen_escape_string(const char *s, StrBuf *out) {
    sb_append_char(out, '"');
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '\n': sb_append(out, "\\n"); break;
            case '\t': sb_append(out, "\\t"); break;
            case '\r': sb_append(out, "\\r"); break;
            case '\\': sb_append(out, "\\\\"); break;
            case '"':  sb_append(out, "\\\""); break;
            case '%':  sb_append(out, "%%"); break; /* printf 格式 */
            default:
                if (c >= 0x80) {
                    // UTF-8字节直接输出
                    sb_appendf(out, "\\x%02x", c);
                } else if (c >= 32 && c < 127) {
                    sb_append_char(out, c);
                } else {
                    sb_appendf(out, "\\x%02x", c);
                }
        }
    }
    sb_append_char(out, '"');
}

/* 前向声明 */
static void cgen_expr(AstNode *node, StrBuf *out);
static void cgen_stmt(AstNode *node, StrBuf *out);

/* 生成表达式 */
static void cgen_expr(AstNode *node, StrBuf *out) {
    if (!node) { sb_append(out, "0"); return; }

    switch (node->type) {
        case AST_INT_LIT:
            sb_appendf(out, "%lldL", (long long)node->int_val);
            break;

        case AST_FLOAT_LIT:
            sb_appendf(out, "%f", node->float_val);
            break;

        case AST_STRING_LIT:
            cgen_escape_string(node->str_val, out);
            break;

        case AST_CHAR_LIT:
            sb_appendf(out, "'\\x%02x'", (unsigned)(node->int_val & 0xFF));
            break;

        case AST_BOOL_LIT:
            sb_append(out, node->int_val ? "1" : "0");
            break;

        case AST_NIL_LIT:
            sb_append(out, "NULL");
            break;

        case AST_IDENT: {
            /* Check if this is an enum variant by original Chinese name */
            { const char *_ecname = enum_variant_cname(node->name);
              if (_ecname && strcmp(_ecname, node->name) != 0) { sb_append(out, _ecname); break; }
            }
            char cname[MAX_TOKEN_LEN];
            ident_to_c(node->name, cname, sizeof(cname));
            sb_append(out, cname);
            break;
        }

        case AST_BINOP: {
            sb_append_char(out, '(');
            cgen_expr(node->child[0], out);
            sb_append_char(out, ' ');

            switch (node->op) {
                case TK_PLUS: case TK_CN_PLUS:    sb_append(out, "+"); break;
                case TK_MINUS: case TK_CN_MINUS:  sb_append(out, "-"); break;
                case TK_STAR: case TK_CN_STAR:    sb_append(out, "*"); break;
                case TK_SLASH: case TK_CN_SLASH:  sb_append(out, "/"); break;
                case TK_PERCENT:                   sb_append(out, "%"); break;
                case TK_EQ_EQ: case TK_CN_EQ:     sb_append(out, "=="); break;
                case TK_BANG_EQ: case TK_CN_NEQ:  sb_append(out, "!="); break;
                case TK_LT: case TK_CN_LT:         sb_append(out, "<"); break;
                case TK_LE: case TK_CN_LE:         sb_append(out, "<="); break;
                case TK_GT: case TK_CN_GT:         sb_append(out, ">"); break;
                case TK_GE: case TK_CN_GE:         sb_append(out, ">="); break;
                case TK_AND: case TK_CN_AND:       sb_append(out, "&&"); break;
                case TK_OR: case TK_CN_OR:         sb_append(out, "||"); break;
                case TK_BIT_AND:                   sb_append(out, "&"); break;
                case TK_BIT_OR:                    sb_append(out, "|"); break;
                case TK_BIT_XOR:                    sb_append(out, "^"); break;
                case TK_SHL:                        sb_append(out, "<<"); break;
                case TK_SHR:                        sb_append(out, ">>"); break;
                default: sb_append(out, "+"); break;
            }
            sb_append_char(out, ' ');
            cgen_expr(node->child[1], out);
            sb_append_char(out, ')');
            break;
        }

        case AST_UNOP: {
            sb_append_char(out, '(');
            switch (node->op) {
                case TK_MINUS: case TK_CN_MINUS:  sb_append(out, "-("); break;
                case TK_BANG: case TK_CN_NOT:      sb_append(out, "!("); break;
                case TK_BIT_NOT:                    sb_append(out, "~("); break;
                default: sb_append(out, "("); break;
            }
            cgen_expr(node->child[0], out);
            sb_append(out, ")");
            break;
        }

        case AST_ASSIGN: {
            cgen_expr(node->child[0], out);
            sb_append(out, " = ");
            cgen_expr(node->child[1], out);
            break;
        }

        case AST_CALL: {
            AstNode *callee = node->child[0];
            cgen_expr(callee, out);
            sb_append_char(out, '(');
            for (int i = 0; i < node->children.count; i++) {
                if (i > 0) sb_append(out, ", ");
                cgen_expr(node->children.items[i], out);
            }
            sb_append_char(out, ')');
            break;
        }

        case AST_MEMBER_ACCESS: {
            cgen_expr(node->child[0], out);
            sb_append_char(out, '.');
            char cname[MAX_TOKEN_LEN];
            ident_to_c(node->name, cname, sizeof(cname));
            sb_append(out, cname);
            break;
        }

        case AST_INDEX: {
            cgen_expr(node->child[0], out);
            sb_append_char(out, '[');
            cgen_expr(node->child[1], out);
            sb_append_char(out, ']');
            break;
        }

        case AST_ARRAY_LIT: {
            // C 数组初始化: (type[]){...}
            sb_append(out, "(long[]){" );
            for (int i = 0; i < node->children.count; i++) {
                if (i > 0) sb_append(out, ", ");
                cgen_expr(node->children.items[i], out);
            }
            sb_append(out, "}");
            // 附加长度信息
            sb_appendf(out, " /* len=%d */", node->children.count);
            break;
        }

        case AST_NEW_EXPR: {
            char cname[MAX_TOKEN_LEN];
            ident_to_c(node->name, cname, sizeof(cname));
            if (node->children.count > 0) {
                // 结构体初始化: malloc + 字段赋值
                sb_appendf(out, "({ struct %s *_o = malloc(sizeof(struct %s)); ", cname, cname);
                for (int i = 0; i < node->children.count; i++) {
                    AstNode *field = node->children.items[i];
                    char fname[MAX_TOKEN_LEN];
                    ident_to_c(field->name, fname, sizeof(fname));
                    sb_appendf(out, "_o->%s = ", fname);
                    cgen_expr(field->child[0], out);
                    sb_append(out, "; ");
                }
                sb_append(out, "_o; })");
            } else {
                sb_appendf(out, "malloc(sizeof(struct %s))", cname);
            }
            break;
        }

        case AST_CAST: {
            sb_append_char(out, '(');
            const char *ct = yao_type_to_c(node->type_name);
            sb_append(out, ct);
            sb_append(out, ")(");
            cgen_expr(node->child[0], out);
            sb_append(out, ")");
            break;
        }

        case AST_REF_EXPR: {
            sb_append_char(out, '&');
            cgen_expr(node->child[0], out);
            break;
        }

        case AST_DEREF_EXPR: {
            sb_append_char(out, '*');
            cgen_expr(node->child[0], out);
            break;
        }

        case AST_SIZEOF_EXPR: {
            sb_append(out, "sizeof(");
            if (node->child[0]) {
                const char *ct = yao_type_to_c(((AstNode*)node->child[0])->name);
                sb_append(out, ct);
            }
            sb_append(out, ")");
            break;
        }

        case AST_TRY_EXPR: {
            // try expr → 简化: 直接生成表达式
            cgen_expr(node->child[0], out);
            break;
        }

        case AST_THROW_EXPR: {
            sb_append(out, "(fprintf(stderr, \"曜语异常\"), exit(1), 0)");
            break;
        }

        case AST_AWAIT_EXPR: {
            cgen_expr(node->child[0], out);
            break;
        }

        default:
            sb_append(out, "/* 未知表达式 */ 0");
            break;
    }
}

/* 生成语句 */
static void cgen_stmt(AstNode *node, StrBuf *out) {
    if (!node) return;

    switch (node->type) {
        case AST_BLOCK: {
            sb_append(out, "{\n");
            for (int i = 0; i < node->children.count; i++) {
                cgen_stmt(node->children.items[i], out);
                sb_append_char(out, '\n');
            }
            sb_append(out, "}\n");
            break;
        }

        case AST_EXPR_STMT: {
            cgen_expr(node->child[0], out);
            sb_append_char(out, ';');
            break;
        }

        case AST_VAR_DECL: {
            const char *ctype = "long";
            if (node->type_name[0]) {
                ctype = yao_type_to_c(node->type_name);
            }
            // 如果有初始化表达式且类型是文本, 需要特殊处理
            if (node->child[0] && node->child[0]->type == AST_STRING_LIT) {
                // 推断为 char*
                if (!node->type_name[0]) ctype = "char*";
            }
            // 如果有初始化表达式且类型是浮点
            if (node->child[0] && node->child[0]->type == AST_FLOAT_LIT) {
                if (!node->type_name[0]) ctype = "double";
            }

            char cname[MAX_TOKEN_LEN];
            ident_to_c(node->name, cname, sizeof(cname));

            // struct 类型特殊处理
            if (strncmp(ctype, "struct", 6) == 0 || (node->type_name[0] && strcmp(node->type_name, "整数") != 0 && strcmp(node->type_name, "浮点") != 0 && strcmp(node->type_name, "布尔") != 0 && strcmp(node->type_name, "文本") != 0 && strcmp(node->type_name, "字符") != 0 && strcmp(node->type_name, "空类型") != 0 && node->type_name[0])) {
                // 用户类型
                char tname[MAX_TOKEN_LEN];
                ident_to_c(node->type_name, tname, sizeof(tname));
                if (types_find(node->type_name) && types_find(node->type_name)->is_struct) {
                    sb_appendf(out, "struct %s %s", tname, cname);
                } else {
                    sb_appendf(out, "%s %s", tname, cname);
                }
            } else {
                sb_appendf(out, "%s %s", ctype, cname);
            }

            if (node->child[0]) {
                sb_append(out, " = ");
                cgen_expr(node->child[0], out);
            }
            sb_append_char(out, ';');
            // 注册到符号表
            symtab_add(cname, node->type_name, !node->is_mut, false);
            break;
        }

        case AST_IF_STMT: {
            char *else_label = new_label();
            char *end_label = new_label();
            sb_append(out, "if (!(");
            cgen_expr(node->child[0], out);
            sb_appendf(out, ")) goto %s;\n", else_label);
            sb_append(out, "{\n");
            cgen_stmt(node->child[1], out);
            sb_append(out, "} ");
            sb_appendf(out, "goto %s;\n", end_label);
            sb_appendf(out, "%s: {", else_label);
            if (node->child[2]) {
                sb_append_char(out, '\n');
                cgen_stmt(node->child[2], out);
            }
            sb_appendf(out, "}\n%s:\n", end_label);
            break;
        }

        case AST_WHILE_STMT: {
            char *start_label = new_label();
            char *end_label = new_label();
            sb_appendf(out, "%s:", start_label);
            sb_append(out, " if (!(");
            cgen_expr(node->child[0], out);
            sb_appendf(out, ")) goto %s;\n", end_label);
            sb_append(out, "{\n");
            // 保存 break/continue 标签
            int saved_depth = g_depth;
            g_depth++;
            push_loop(end_label, start_label);
            cgen_stmt(node->child[1], out);
            g_depth = saved_depth;
            pop_loop();
            sb_append(out, "}\n");
            sb_appendf(out, "goto %s;\n", start_label);
            sb_appendf(out, "%s:\n", end_label);
            break;
        }

        case AST_LOOP_STMT: {
            char *start_label = new_label();
            char *end_label = new_label();
            sb_appendf(out, "%s:\n{", start_label);
            int saved_depth = g_depth;
            g_depth++;
            cgen_stmt(node->child[0], out);
            g_depth = saved_depth;
            sb_append(out, "}\n");
            sb_appendf(out, "goto %s;\n%s:\n", start_label, end_label);
            break;
        }

        case AST_FOR_STMT: {
            // 对于 i 在 range { body }
            // 生成: for (long i = start; i < end; i++) { body }
            char cname[MAX_TOKEN_LEN];
            ident_to_c(node->name, cname, sizeof(cname));

            AstNode *range = node->child[0];
            if (range && range->type == AST_BINOP && range->op == TK_DOTDOT) {
                // 范围表达式: a..b
                sb_appendf(out, "for (long %s = ", cname);
                cgen_expr(range->child[0], out);
                sb_appendf(out, "; %s < ", cname);
                cgen_expr(range->child[1], out);
                sb_appendf(out, "; %s++) {\n", cname);
            } else {
                // 集合遍历: 简化为索引遍历
                sb_appendf(out, "for (long %s = 0; %s < 100; %s++) {\n", cname, cname, cname);
            }
            int saved_depth = g_depth;
            g_depth++;
            cgen_stmt(node->child[1], out);
            g_depth = saved_depth;
            sb_append(out, "}\n");
            break;
        }

        case AST_BREAK: {
            if (g_brk_top > 0) { char _bl[64]; snprintf(_bl,64,"goto %s;",g_brk_label[g_brk_top-1]); sb_append(out,_bl); }
            else sb_append(out, "break;");
            break;
        }

        case AST_CONTINUE: {
            if (g_cont_top > 0) { char _cl[64]; snprintf(_cl,64,"goto %s;",g_cont_label[g_cont_top-1]); sb_append(out,_cl); }
            else sb_append(out, "continue;");
            break;
        }

        case AST_RETURN:
            sb_append(out, "return ");
            if (node->child[0]) {
                cgen_expr(node->child[0], out);
            } else {
                sb_append(out, "0");
            }
            sb_append_char(out, ';');
            break;

        case AST_MATCH_STMT: {
            /* 简化为 if-else 链 */
            char match_var_name[32];
            snprintf(match_var_name, sizeof(match_var_name), "_match_%d", g_tmp_counter++);
            sb_appendf(out, "long %s = ", match_var_name);
            cgen_expr(node->child[0], out);
            sb_appendf(out, ";\n");
            for (int i = 0; i < node->match_arm_count; i++) {
                AstNode *pat = node->match_arms[i].pattern;
                AstNode *body = node->match_arms[i].body;
                if (pat && pat->type == AST_IDENT && strcmp(pat->name, "_") == 0) {
                    sb_appendf(out, "{ ");
                    cgen_expr(body, out);
                    sb_appendf(out, "; }\n");
                    break;
                } else if (pat && pat->type == AST_INT_LIT) {
                    sb_appendf(out, "%s==%lldLL ? ({ ", match_var_name, (long long)pat->int_val);
                    cgen_expr(body, out);
                    sb_append(out, "; }) : ");
                } else if (pat && pat->type == AST_IDENT) {
                    sb_appendf(out, "%s==%s ? ({ ", match_var_name, pat->name);
                    cgen_expr(body, out);
                    sb_append(out, "; }) : ");
                } else {
                    sb_appendf(out, "({ ");
                    if (body) cgen_expr(body, out);
                    sb_append(out, "; }) : ");
                }
            }
            sb_append(out, "0;\n");
            break;
        }

        default:
            sb_append(out, "/* 未知语句 */\n");
            break;
    }
}

/* 生成函数声明 */
static void cgen_fn(AstNode *fn, StrBuf *out) {
    // extern 声明
    if (fn->is_extern) {
        const char *ret_c = yao_type_to_c(fn->ret_type);
        sb_appendf(out, "extern %s ", ret_c);
        char cname[MAX_TOKEN_LEN];
        ident_to_c(fn->name, cname, sizeof(cname));
        sb_append(out, cname);
        sb_append_char(out, '(');
        for (int i = 0; i < fn->param_count; i++) {
            if (i > 0) sb_append(out, ", ");
            sb_append(out, yao_type_to_c(fn->params[i].type_name));
        }
        if (fn->param_count == 0) sb_append(out, "void");
        sb_append(out, ");\n");
        return;
    }

    // 函数定义
    const char *ret_c = yao_type_to_c(fn->ret_type);
    char cname[MAX_TOKEN_LEN];
    ident_to_c(fn->name, cname, sizeof(cname));

    sb_appendf(out, "%s %s(", ret_c, cname);
    for (int i = 0; i < fn->param_count; i++) {
        if (i > 0) sb_append(out, ", ");
        const char *pt = yao_type_to_c(fn->params[i].type_name);
        char pname[MAX_TOKEN_LEN];
        ident_to_c(fn->params[i].name, pname, sizeof(pname));
        sb_appendf(out, "%s %s", pt, pname);
    }
    if (fn->param_count == 0) sb_append(out, "void");
    sb_append(out, ") {\n");

    // 函数体
    symtab_enter_scope();
    for (int i = 0; i < fn->param_count; i++) {
        char pname[MAX_TOKEN_LEN];
        ident_to_c(fn->params[i].name, pname, sizeof(pname));
        symtab_add(pname, fn->params[i].type_name, false, false);
    }
    if (fn->child[0]) {
        cgen_stmt(fn->child[0], out);
    }
    symtab_exit_scope();
    sb_append(out, "}\n\n");
}

/* 生成结构体 */
static void cgen_struct(AstNode *st, StrBuf *out) {
    char cname[MAX_TOKEN_LEN];
    ident_to_c(st->name, cname, sizeof(cname));
    sb_appendf(out, "struct %s {\n", cname);
    for (int i = 0; i < st->member_count; i++) {
        const char *mt = yao_type_to_c(st->members[i].type_name);
        char mname_c[MAX_TOKEN_LEN];
        ident_to_c(st->members[i].name, mname_c, sizeof(mname_c));
        sb_appendf(out, "    %s %s;\n", mt, mname_c);
    }
    sb_append(out, "};\n\n");

    // typedef
    sb_appendf(out, "typedef struct %s %s;\n\n", cname, cname);
}

/* 生成枚举 */
static void cgen_enum(AstNode *en, StrBuf *out) {
    char cname[MAX_TOKEN_LEN];
    ident_to_c(en->name, cname, sizeof(cname));
    sb_appendf(out, "typedef enum %s {\n", cname);
    for (int i = 0; i < en->variant_count; i++) {
        char vname_c[MAX_TOKEN_LEN];
        ident_to_c(en->variants[i].name, vname_c, sizeof(vname_c));
        sb_appendf(out, "    %s_%s = %lld,\n", cname, vname_c, (long long)en->variants[i].value);
    }
    sb_appendf(out, "} %s;\n\n", cname);
}

/* 生成实现块 */
static void cgen_impl(AstNode *im, StrBuf *out) {
    // impl 块中的方法逐一生成
    for (int i = 0; i < im->children.count; i++) {
        AstNode *method = im->children.items[i];
        if (method->type == AST_FN_DECL) {
            // 方法: 第一个参数自动添加 自身 类型指针
            // 生成 方法名(自身* self, ...)
            const char *ret_c = yao_type_to_c(method->ret_type);
            char cname[MAX_TOKEN_LEN];
            ident_to_c(method->name, cname, sizeof(cname));
            char type_cname[MAX_TOKEN_LEN];
            ident_to_c(im->impl_type, type_cname, sizeof(type_cname));

            sb_appendf(out, "%s %s(struct %s* self", ret_c, cname, type_cname);
            for (int j = 0; j < method->param_count; j++) {
                sb_append(out, ", ");
                sb_append(out, yao_type_to_c(method->params[j].type_name));
                char pname[MAX_TOKEN_LEN];
                ident_to_c(method->params[j].name, pname, sizeof(pname));
                sb_appendf(out, " %s", pname);
            }
            sb_append(out, ") {\n");

            symtab_enter_scope();
            symtab_add("self", im->impl_type, false, false);
            for (int j = 0; j < method->param_count; j++) {
                char pname[MAX_TOKEN_LEN];
                ident_to_c(method->params[j].name, pname, sizeof(pname));
                symtab_add(pname, method->params[j].type_name, false, false);
            }
            if (method->child[0]) {
                cgen_stmt(method->child[0], out);
            }
            symtab_exit_scope();
            sb_append(out, "}\n\n");
        }
    }
}

/* 生成类型别名 */
static void cgen_type_alias(AstNode *ta, StrBuf *out) {
    const char *real_c = yao_type_to_c(ta->type_name);
    char cname[MAX_TOKEN_LEN];
    ident_to_c(ta->name, cname, sizeof(cname));
    sb_appendf(out, "typedef %s %s;\n", real_c, cname);
}

/* 生成整个程序 */
static void cgen_program(AstNode *prog, StrBuf *out) {
    /* C 头部 */
    sb_append(out,
        "/* ════════════════════════════════════════════════════════════════\n"
        " * 曜语 (YaoLang) 自动生成的 C 代码\n"
        " * 由 曜语编译器 版本 " YVM_VERSION " 产生\n"
        " * 请勿手动编辑\n"
        " * ════════════════════════════════════════════════════════════════ */\n\n"
    );

    sb_append(out,
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include <math.h>\n"
        "#include <stdint.h>\n"
        "#include <stdbool.h>\n"
        "\n"
    );

    /* 内建函数 — 自举级 Runtime */
    sb_append(out,
        "/* 曜语内建函数 — 自举级 Runtime */\n"
        "\n"
        "/* ── 输出 ── */\n"
        "static void _yao_print(long x) { printf(\"%ld\\n\", x); }\n"
        "static void _yao_print_d(double x) { printf(\"%f\\n\", x); }\n"
        "static void _yao_print_s(const char* x) { printf(\"%s\\n\", x); }\n"
        "static void _yao_print_b(int x) { printf(\"%s\\n\", x ? \"真\" : \"假\"); }\n"
        "static void _yao_print_c(char c) { putchar(c); }\n"
        "static void _yao_print_raw(const char* x) { printf(\"%s\", x); }\n"
        "\n"
        "/* ── 输入 ── */\n"
        "static long _yao_input() { long v; scanf(\"%ld\", &v); return v; }\n"
        "static char* _yao_input_s() { char* s = malloc(1024); scanf(\"%1023s\", s); return s; }\n"
        "static int _yao_getchar() { return getchar(); }\n"
        "\n"
        "/* ── 字符串操作 ── */\n"
        "static long _yao_strlen(const char* s) { return (long)strlen(s); }\n"
        "static char* _yao_strcat(const char* a, const char* b) {\n"
        "    long la = strlen(a), lb = strlen(b);\n"
        "    char* r = malloc(la + lb + 1);\n"
        "    memcpy(r, a, la); memcpy(r+la, b, lb); r[la+lb] = 0;\n"
        "    return r;\n"
        "}\n"
        "static char* _yao_strdup(const char* s) {\n"
        "    long n = strlen(s); char* r = malloc(n+1); memcpy(r, s, n+1); return r;\n"
        "}\n"
        "static int _yao_streq(const char* a, const char* b) { return strcmp(a, b) == 0; }\n"
        "static int _yao_strne(const char* a, const char* b) { return strcmp(a, b) != 0; }\n"
        "static long _yao_strcmp(const char* a, const char* b) { return (long)strcmp(a, b); }\n"
        "static char* _yao_substr(const char* s, long start, long len) {\n"
        "    long sl = strlen(s);\n"
        "    if (start < 0) start = 0;\n"
        "    if (start >= sl) { char* r = malloc(1); r[0] = 0; return r; }\n"
        "    if (len < 0 || start + len > sl) len = sl - start;\n"
        "    char* r = malloc(len + 1);\n"
        "    memcpy(r, s + start, len); r[len] = 0;\n"
        "    return r;\n"
        "}\n"
        "static char _yao_char_at(const char* s, long i) {\n"
        "    long sl = strlen(s);\n"
        "    if (i < 0 || i >= sl) return 0;\n"
        "    return s[i];\n"
        "}\n"
        "static char* _yao_itoa(long v) {\n"
        "    char* r = malloc(32); snprintf(r, 32, \"%ld\", v); return r;\n"
        "}\n"
        "static char* _yao_ftoa(double v) {\n"
        "    char* r = malloc(32); snprintf(r, 32, \"%f\", v); return r;\n"
        "}\n"
        "static long _yao_atoi(const char* s) { return atol(s); }\n"
        "static double _yao_atof(const char* s) { return atof(s); }\n"
        "\n"
        "/* ── 字符判断 ── */\n"
        "static int _yao_isdigit(int c) { return c >= '0' && c <= '9'; }\n"
        "static int _yao_isalpha(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }\n"
        "static int _yao_isalnum(int c) { return _yao_isdigit(c) || _yao_isalpha(c); }\n"
        "static int _yao_isspace(int c) { return c==' '||c=='\\t'||c=='\\n'||c=='\\r'; }\n"
        "static int _yao_isupper(int c) { return c >= 'A' && c <= 'Z'; }\n"
        "static int _yao_islower(int c) { return c >= 'a' && c <= 'z'; }\n"
        "static int _yao_toupper(int c) { return (c>='a'&&c<='z') ? c-32 : c; }\n"
        "static int _yao_tolower(int c) { return (c>='A'&&c<='Z') ? c+32 : c; }\n"
        "\n"
        "/* ── 内存/数组 ── */\n"
        "static void* _yao_alloc(long size) { return malloc(size); }\n"
        "static void* _yao_realloc(void* p, long size) { return realloc(p, size); }\n"
        "static void _yao_free(void* p) { free(p); }\n"
        "static long* _yao_arr_new(long n) { return (long*)calloc(n, sizeof(long)); }\n"
        "static long _yao_arr_get(long* arr, long i) { return arr[i]; }\n"
        "static void _yao_arr_set(long* arr, long i, long v) { arr[i] = v; }\n"
        "static void _yao_arr_print(long* arr, long n) { for(long i=0;i<n;i++) printf(\"%ld \", arr[i]); printf(\"\\n\"); }\n"
        "\n"
        "/* ── 文件 I/O ── */\n"
        "static void* _yao_file_open(const char* path, const char* mode) {\n"
        "    return (void*)fopen(path, mode);\n"
        "}\n"
        "static int _yao_file_close(void* fp) { return fclose((FILE*)fp); }\n"
        "static int _yao_file_write(void* fp, const char* s) { return fputs(s, (FILE*)fp); }\n"
        "static char* _yao_file_read(void* fp) {\n"
        "    fseek((FILE*)fp, 0, SEEK_END); long sz = ftell((FILE*)fp); fseek((FILE*)fp, 0, SEEK_SET);\n"
        "    char* buf = malloc(sz + 1);\n"
        "    long rd = fread(buf, 1, sz, (FILE*)fp); buf[rd] = 0;\n"
        "    return buf;\n"
        "}\n"
        "static long _yao_file_size(void* fp) {\n"
        "    long cur = ftell((FILE*)fp); fseek((FILE*)fp, 0, SEEK_END); long sz = ftell((FILE*)fp); fseek((FILE*)fp, cur, SEEK_SET);\n"
        "    return sz;\n"
        "}\n"
        "static char* _yao_read_file(const char* path) {\n"
        "    FILE* f = fopen(path, \"rb\");\n"
        "    if (!f) { char* r = malloc(1); r[0] = 0; return r; }\n"
        "    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);\n"
        "    char* buf = malloc(sz + 1);\n"
        "    long rd = fread(buf, 1, sz, f); buf[rd] = 0;\n"
        "    fclose(f);\n"
        "    return buf;\n"
        "}\n"
        "static int _yao_file_exists(const char* path) {\n"
        "    FILE* f = fopen(path, \"r\");\n"
        "    if (f) { fclose(f); return 1; } return 0;\n"
        "}\n"
        "static int _yao_write_file(const char* path, const char* content) {\n"
        "    FILE* f = fopen(path, \"wb\");\n"
        "    if (!f) return 0;\n"
        "    fputs(content, f);\n"
        "    fclose(f);\n"
        "    return 1;\n"
        "}\n"
        "static int _yao_system(const char* cmd) { return system(cmd); }\n"
        "\n"
        "/* ── 数学 ── */\n"
        "static long _yao_abs(long x) { return x < 0 ? -x : x; }\n"
        "static double _yao_sqrt_d(double x) { return sqrt(x); }\n"
        "static double _yao_pow_d(double a, double b) { return pow(a, b); }\n"
        "static double _yao_sin_d(double x) { return sin(x); }\n"
        "static double _yao_cos_d(double x) { return cos(x); }\n"
        "static double _yao_tan_d(double x) { return tan(x); }\n"
        "static double _yao_floor_d(double x) { return floor(x); }\n"
        "static double _yao_ceil_d(double x) { return ceil(x); }\n"
        "static long _yao_max(long a, long b) { return a > b ? a : b; }\n"
        "static long _yao_min(long a, long b) { return a < b ? a : b; }\n"
        "\n"
        "/* ── 退出 ── */\n"
        "static void _yao_exit(int code) { exit(code); }\n"
        "\n"
        "/* ── X11 GUI 绑定 (需要 -lX11 链接) ── */\n"
        "#ifdef HAS_X11\n"
        "#include <X11/Xlib.h>\n"
        "#include <X11/Xutil.h>\n"
        "static Display* _yao_x11_display = NULL;\n"
        "static int _yao_x11_screen = 0;\n"
        "static void _yao_x11_init() {\n"
        "    if (!_yao_x11_display) {\n"
        "        _yao_x11_display = XOpenDisplay(NULL);\n"
        "        if (_yao_x11_display) _yao_x11_screen = DefaultScreen(_yao_x11_display);\n"
        "    }\n"
        "}\n"
        "static long _yao_x11_create_window(long w, long h, const char* title) {\n"
        "    _yao_x11_init();\n"
        "    if (!_yao_x11_display) return 0;\n"
        "    Window win = XCreateSimpleWindow(_yao_x11_display,\n"
        "        RootWindow(_yao_x11_display, _yao_x11_screen),\n"
        "        0, 0, w, h, 1,\n"
        "        BlackPixel(_yao_x11_display, _yao_x11_screen),\n"
        "        WhitePixel(_yao_x11_display, _yao_x11_screen));\n"
        "    XStoreName(_yao_x11_display, win, title);\n"
        "    XSelectInput(_yao_x11_display, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);\n"
        "    XMapWindow(_yao_x11_display, win);\n"
        "    XFlush(_yao_x11_display);\n"
        "    return (long)win;\n"
        "}\n"
        "static void _yao_x11_draw_text(long win, long x, long y, const char* text) {\n"
        "    GC gc = XCreateGC(_yao_x11_display, win, 0, NULL);\n"
        "    XDrawString(_yao_x11_display, win, gc, x, y, text, strlen(text));\n"
        "    XFreeGC(_yao_x11_display, gc);\n"
        "    XFlush(_yao_x11_display);\n"
        "}\n"
        "static void _yao_x11_draw_rect(long win, long x, long y, long w, long h) {\n"
        "    GC gc = XCreateGC(_yao_x11_display, win, 0, NULL);\n"
        "    XDrawRectangle(_yao_x11_display, win, gc, x, y, w, h);\n"
        "    XFreeGC(_yao_x11_display, gc);\n"
        "    XFlush(_yao_x11_display);\n"
        "}\n"
        "static void _yao_x11_fill_rect(long win, long x, long y, long w, long h) {\n"
        "    GC gc = XCreateGC(_yao_x11_display, win, 0, NULL);\n"
        "    XFillRectangle(_yao_x11_display, win, gc, x, y, w, h);\n"
        "    XFreeGC(_yao_x11_display, gc);\n"
        "    XFlush(_yao_x11_display);\n"
        "}\n"
        "static void _yao_x11_draw_line(long win, long x1, long y1, long x2, long y2) {\n"
        "    GC gc = XCreateGC(_yao_x11_display, win, 0, NULL);\n"
        "    XDrawLine(_yao_x11_display, win, gc, x1, y1, x2, y2);\n"
        "    XFreeGC(_yao_x11_display, gc);\n"
        "    XFlush(_yao_x11_display);\n"
        "}\n"
        "static void _yao_x11_draw_circle(long win, long cx, long cy, long r) {\n"
        "    GC gc = XCreateGC(_yao_x11_display, win, 0, NULL);\n"
        "    XDrawArc(_yao_x11_display, win, gc, cx-r, cy-r, 2*r, 2*r, 0, 360*64);\n"
        "    XFreeGC(_yao_x11_display, gc);\n"
        "    XFlush(_yao_x11_display);\n"
        "}\n"
        "static void _yao_x11_set_color(long win, long color) {\n"
        "    (void)win; (void)color;\n"
        "}\n"
        "static long _yao_x11_next_event(long win) {\n"
        "    XEvent ev;\n"
        "    XNextEvent(_yao_x11_display, &ev);\n"
        "    if (ev.type == KeyPress) return 1;\n"
        "    if (ev.type == ButtonPress) return 2;\n"
        "    if (ev.type == Expose) return 3;\n"
        "    return 0;\n"
        "}\n"
        "static long _yao_x11_event_x() { return 0; }\n"
        "static long _yao_x11_event_y() { return 0; }\n"
        "static void _yao_x11_close() {\n"
        "    if (_yao_x11_display) { XCloseDisplay(_yao_x11_display); _yao_x11_display = NULL; }\n"
        "}\n"
        "static void _yao_x11_loop(long win) {\n"
        "    while (1) {\n"
        "        XEvent ev;\n"
        "        XNextEvent(_yao_x11_display, &ev);\n"
        "        if (ev.type == KeyPress) break;\n"
        "        if (ev.type == ClientMessage) break;\n"
        "    }\n"
        "    (void)win;\n"
        "}\n"
        "static void _yao_x11_clear(long win) {\n"
        "    XClearWindow(_yao_x11_display, win);\n"
        "    XFlush(_yao_x11_display);\n"
        "}\n"
        "static void _yao_x11_flush() {\n"
        "    if (_yao_x11_display) XFlush(_yao_x11_display);\n"
        "}\n"
        "#endif /* HAS_X11 */\n"

    );

    // 所有全局变量: 只声明，非常量初始化器延迟到 main
    StrBuf init_stmts;  // 需要在 main 开头执行的初始化语句
    sb_init(&init_stmts);

    /* 类型别名先于函数 */
    for (int i = 0; i < prog->children.count; i++) {
        AstNode *decl = prog->children.items[i];
        if (decl->type == AST_TYPE_ALIAS) cgen_type_alias(decl, out);
    }

    for (int i = 0; i < prog->children.count; i++) {
        AstNode *decl = prog->children.items[i];
        if (decl->type == AST_ENUM_DECL) cgen_enum(decl, out);
    }

    for (int i = 0; i < prog->children.count; i++) {
        AstNode *decl = prog->children.items[i];
        if (decl->type == AST_STRUCT_DECL) cgen_struct(decl, out);
    }

    /* extern 声明 */
    for (int i = 0; i < prog->children.count; i++) {
        AstNode *decl = prog->children.items[i];
        if (decl->type == AST_FN_DECL && decl->is_extern) cgen_fn(decl, out);
    }

    /* 函数前向声明 */
    for (int i = 0; i < prog->children.count; i++) {
        AstNode *decl = prog->children.items[i];
        if (decl->type == AST_FN_DECL && !decl->is_extern) {
            const char *ret_c = yao_type_to_c(decl->ret_type);
            char cname[MAX_TOKEN_LEN];
            ident_to_c(decl->name, cname, sizeof(cname));
            sb_appendf(out, "%s %s(", ret_c, cname);
            for (int j = 0; j < decl->param_count; j++) {
                if (j > 0) sb_append(out, ", ");
                sb_append(out, yao_type_to_c(decl->params[j].type_name));
            }
            if (decl->param_count == 0) sb_append(out, "void");
            sb_append(out, ");\n");
        }
    }
    sb_append_char(out, '\n');

    /* 全局变量: 只声明, 非常量初始化器延迟 */
    for (int i = 0; i < prog->children.count; i++) {
        AstNode *decl = prog->children.items[i];
        if (decl->type == AST_VAR_DECL) {
            const char *ctype = "long";
            if (decl->type_name[0]) {
                ctype = yao_type_to_c(decl->type_name);
            }
            if (decl->child[0] && decl->child[0]->type == AST_STRING_LIT) {
                if (!decl->type_name[0]) ctype = "char*";
            }
            if (decl->child[0] && decl->child[0]->type == AST_FLOAT_LIT) {
                if (!decl->type_name[0]) ctype = "double";
            }

            char cname[MAX_TOKEN_LEN];
            ident_to_c(decl->name, cname, sizeof(cname));

            // 判断初始化器是否是常量字面值
            bool init_is_const = false;
            if (decl->child[0]) {
                AstType t = decl->child[0]->type;
                if (t == AST_INT_LIT || t == AST_FLOAT_LIT || t == AST_STRING_LIT ||
                    t == AST_BOOL_LIT || t == AST_CHAR_LIT || t == AST_NIL_LIT) {
                    init_is_const = true;
                }
            }

            // struct 类型特殊处理
            if (strncmp(ctype, "struct", 6) == 0 || (decl->type_name[0] && strcmp(decl->type_name, "整数") != 0 && strcmp(decl->type_name, "浮点") != 0 && strcmp(decl->type_name, "布尔") != 0 && strcmp(decl->type_name, "文本") != 0 && strcmp(decl->type_name, "字符") != 0 && strcmp(decl->type_name, "空类型") != 0 && decl->type_name[0])) {
                char tname[MAX_TOKEN_LEN];
                ident_to_c(decl->type_name, tname, sizeof(tname));
                if (types_find(decl->type_name) && types_find(decl->type_name)->is_struct) {
                    sb_appendf(out, "struct %s %s", tname, cname);
                } else {
                    sb_appendf(out, "%s %s", tname, cname);
                }
            } else {
                sb_appendf(out, "%s %s", ctype, cname);
            }

            if (decl->child[0] && init_is_const) {
                // 常量初始化器可以直接在全局区
                sb_append(out, " = ");
                cgen_expr(decl->child[0], out);
            } else if (decl->child[0] && !init_is_const) {
                // 非常量初始化器: 在 main 中执行
                sb_append(&init_stmts, "    ");
                sb_append(&init_stmts, cname);
                sb_append(&init_stmts, " = ");
                StrBuf tmp; sb_init(&tmp);
                cgen_expr(decl->child[0], &tmp);
                sb_append(&init_stmts, tmp.data);
                sb_append(&init_stmts, ";\n");
                sb_free(&tmp);
            }
            sb_append(out, ";\n");
            // 注册符号
            symtab_add(cname, decl->type_name, !decl->is_mut, false);
        }
    }
    sb_append_char(out, '\n');

    /* impl 块 (方法) */
    for (int i = 0; i < prog->children.count; i++) {
        AstNode *decl = prog->children.items[i];
        if (decl->type == AST_IMPL_DECL) cgen_impl(decl, out);
    }

    /* 函数定义 */
    for (int i = 0; i < prog->children.count; i++) {
        AstNode *decl = prog->children.items[i];
        if (decl->type == AST_FN_DECL && !decl->is_extern) {
            cgen_fn(decl, out);
        }
    }

    /* main 函数: 将无主函数的顶层语句直接生成 */
    bool has_main = false;
    for (int i = 0; i < prog->children.count; i++) {
        AstNode *decl = prog->children.items[i];
        if (decl->type == AST_FN_DECL && strcmp(decl->name, "主函数") == 0) {
            has_main = true;
            break;
        }
    }

    if (!has_main) {
        sb_append(out, "int main(int argc, char** argv) {\n");
        sb_append(out, init_stmts.data);
        for (int i = 0; i < prog->children.count; i++) {
            AstNode *decl = prog->children.items[i];
            // 生成语句类型的顶层代码
            if (decl->type == AST_EXPR_STMT || decl->type == AST_IF_STMT ||
                decl->type == AST_WHILE_STMT || decl->type == AST_LOOP_STMT ||
                decl->type == AST_FOR_STMT || decl->type == AST_RETURN ||
                decl->type == AST_BREAK || decl->type == AST_CONTINUE ||
                decl->type == AST_MATCH_STMT || decl->type == AST_BLOCK) {
                cgen_stmt(decl, out);
                sb_append_char(out, '\n');
            }
        }
        sb_append(out, "    return 0;\n}\n");
    } else {
        sb_append(out,
            "\n/* 曜语入口点 */\n"
            "int main(int argc, char** argv) {\n"
            "    long _err = setjmp(_yao_main_jmp);\n"
            "    if (_err) { fprintf(stderr, \"曜语: 未捕获异常\\n\"); return 1; }\n"
        );
        // 输出延迟初始化语句
        sb_append(out, init_stmts.data);
        // 顶层语句(在主函数之前定义的语句)
        for (int i = 0; i < prog->children.count; i++) {
            AstNode *decl = prog->children.items[i];
            if (decl->type == AST_EXPR_STMT || decl->type == AST_IF_STMT ||
                decl->type == AST_WHILE_STMT || decl->type == AST_LOOP_STMT ||
                decl->type == AST_FOR_STMT || decl->type == AST_MATCH_STMT) {
                cgen_stmt(decl, out);
                sb_append_char(out, '\n');
            }
        }
        // 调用主函数
        for (int i = 0; i < prog->children.count; i++) {
            AstNode *decl = prog->children.items[i];
            if (decl->type == AST_FN_DECL && strcmp(decl->name, "主函数") == 0) {
                char cname[MAX_TOKEN_LEN];
                ident_to_c(decl->name, cname, sizeof(cname));
                sb_appendf(out, "    %s();\n", cname);
                break;
            }
        }
        sb_append(out, "    return 0;\n}\n");
    }

    sb_free(&init_stmts);
}

/* ════════════════════════════════════════════════════════════════════
 *  文件读取
 * ════════════════════════════════════════════════════════════════════ */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    if (out_len) *out_len = len;
    return buf;
}

/* ════════════════════════════════════════════════════════════════════
 *  setjmp 支持 (异常处理)
 * ════════════════════════════════════════════════════════════════════ */
static void cgen_add_setjmp_header(StrBuf *out) {
    sb_append(out, "#include <setjmp.h>\n");
    sb_append(out, "static jmp_buf _yao_main_jmp;\n\n");
}

/* ════════════════════════════════════════════════════════════════════
 *  主函数 (编译器驱动)
 * ════════════════════════════════════════════════════════════════════ */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "╔═══════════════════════════════════════════════════╗\n"
        "║          曜语 (YaoLang) 编译器 %s          ║\n"
        "╚═══════════════════════════════════════════════════╝\n"
        "\n"
        "用法: %s [选项] <源文件.耀>\n"
        "\n"
        "文件扩展名: .耀 (.yao 也兼容)\n"
        "\n"
        "选项:\n"
        "  -o <文件>    指定输出文件名\n"
        "  -c           只编译不链接 (生成 .c 文件)\n"
        "  -S           只生成 C 代码到 stdout\n"
        "  --lex        只运行词法分析, 输出词元流\n"
        "  --ast        输出抽象语法树\n"
        "  -O<级别>     传递给 C 编译器的优化级别 (0-3)\n"
        "  -v           显示编译器版本\n"
        "  -h           显示帮助\n"
        "\n"
        "示例:\n"
        "  %s hello.耀 -o hello     编译耀语源文件\n"
        "  %s hello.耀 -S           查看生成的C代码\n"
        "  %s hello.耀 --lex        查看词元流\n"
        "\n"
        "中文编程, 曜语同行!  曜历元年\n",
        YVM_VERSION, prog, prog, prog, prog
    );
}

static void print_tokens(Token *tokens, int count) {
    const char *type_names[] = {
        "整数", "浮点", "文本", "字符", "真", "假",
        "标识符", "函数", "变量", "令", "可变", "常量",
        "若", "否则", "否则若", "当", "循环", "对于", "在",
        "跳出", "继续", "返回", "匹配", "结构", "枚举", "特质", "实现",
        "公开", "私有", "自身", "新建", "删除", "作为", "引入", "模块",
        "类型", "引用", "转移", "借用", "不安全", "外部", "异步", "等待",
        "抛出", "尝试", "捕获", "静态", "空", "取大小", "装箱",
        "整数类型", "浮点类型", "布尔类型", "文本类型", "字符类型", "空类型",
        "加", "加等", "减", "减等", "乘", "乘等", "除", "除等", "模", "模等",
        "赋值", "等于", "非", "不等", "小于", "小于等于", "大于", "大于等于",
        "且", "或", "位与", "位或", "异或", "位反", "左移", "右移",
        "箭头", "大箭头", "点", "范围", "省略号",
        "逗号", "冒号", "双冒号", "分号",
        "左括", "右括", "左花", "右花", "左方", "右方", "问号",
        "中文加", "中文减", "中文乘", "中文除", "中文赋值",
        "中文等", "中文不等", "中文小于", "中文小于等于", "中文大于", "中文大于等于",
        "中文且", "中文或", "中文非", "中文点", "中文箭头",
        "文件末尾", "无效"
    };

    for (int i = 0; i < count; i++) {
        Token *t = &tokens[i];
        const char *tn = (t->type < sizeof(type_names)/sizeof(type_names[0]))
            ? type_names[t->type] : "未知";
        if (t->type == TK_EOF) break;
        printf("[%s:%d:%d] %s: '%s'",
            t->file ? t->file : "?", t->line, t->col,
            tn,
            t->text[0] ? t->text : "");
        if (t->type == TK_INT) printf(" (值=%lld)", (long long)t->int_val);
        if (t->type == TK_FLOAT) printf(" (值=%f)", t->float_val);
        printf("\n");
    }
}

static void print_ast(AstNode *node, int indent) {
    if (!node) return;
    for (int i = 0; i < indent; i++) printf("  ");
    const char *type_names[] = {
        "程序", "函数声明", "变量声明", "结构体声明", "枚举声明", "特质声明",
        "实现声明", "引入声明", "模块声明", "类型别名", "外部声明",
        "表达式语句", "块", "如果语句", "当语句", "循环语句", "对于语句",
        "跳出", "继续", "返回", "赋值", "匹配语句",
        "整数字面", "浮点字面", "文本字面", "字符字面", "布尔字面", "空字面",
        "标识符", "二元运算", "一元运算", "调用", "成员访问", "索引",
        "数组字面", "初始化列表", "类型引用", "类型转换",
        "引用表达式", "解引用", "新建表达式", "闭包",
        "取大小表达式", "尝试表达式", "抛出表达式", "等待表达式",
    };
    const char *tn = node->type < sizeof(type_names)/sizeof(type_names[0]) ? type_names[node->type] : "未知";
    printf("%s", tn);
    if (node->name[0]) printf(" name='%s'", node->name);
    if (node->type_name[0]) printf(" type='%s'", node->type_name);
    if (node->str_val[0]) printf(" val='%s'", node->str_val);
    if (node->int_val) printf(" int=%lld", (long long)node->int_val);
    printf("\n");

    // 子节点
    for (int i = 0; i < 8; i++) {
        if (node->child[i]) print_ast(node->child[i], indent + 1);
    }
    for (int i = 0; i < node->children.count; i++) {
        print_ast(node->children.items[i], indent + 1);
    }
}

int main(int argc, char **argv) {
    const char *input_file = NULL;
    const char *output_file = NULL;
    bool only_c = false;       // -c: 只生成C文件
    bool only_stdout = false;   // -S: C代码到stdout
    bool only_lex = false;      // --lex
    bool only_ast = false;      // --ast
    int opt_level = 2;
    bool show_help = false;
    bool show_version = false;

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            show_help = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            show_version = true;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0) {
            only_c = true;
        } else if (strcmp(argv[i], "-S") == 0) {
            only_stdout = true;
        } else if (strcmp(argv[i], "--lex") == 0) {
            only_lex = true;
        } else if (strcmp(argv[i], "--ast") == 0) {
            only_ast = true;
        } else if (strncmp(argv[i], "-O", 2) == 0) {
            opt_level = atoi(argv[i] + 2);
            if (opt_level < 0) opt_level = 0;
            if (opt_level > 3) opt_level = 3;
        } else if (argv[i][0] != '-') {
            input_file = argv[i];
        }
    }

    if (show_version) {
        printf("曜语 (YaoLang) 编译器 版本 %s\n", YVM_VERSION);
        printf("中文编程, 曜语同行!\n");
        return 0;
    }

    if (show_help || !input_file) {
        print_usage(argv[0]);
        return show_help ? 0 : 1;
    }

    /* 检查文件扩展名 */
    if (!is_yao_file(input_file)) {
        fprintf(stderr, "曜语编译器: 文件 '%s' 扩展名不是 .yao 或 .耀\n", input_file);
        return 1;
    }

    /* 读取源文件 */
    size_t src_len = 0;
    g_current_file = input_file;
    char *source = read_file(input_file, &src_len);
    if (!source) {
        fprintf(stderr, "曜语编译器: 无法读取文件 '%s': %s\n", input_file, strerror(errno));
        return 1;
    }

    /* 词法分析 */
    Lexer lx = {0};
    lx.src = source;
    lx.src_len = src_len;
    lx.pos = 0;
    lx.line = 1;
    lx.col = 1;
    lx.file = input_file;
    lx.tokens = NULL;
    lx.token_count = 0;
    lx.token_cap = 0;
    lex(&lx);

    if (only_lex) {
        print_tokens(lx.tokens, lx.token_count);
        if (g_error_count > 0) {
            print_errors();
            return 1;
        }
        free(lx.tokens);
        free(source);
        return 0;
    }

    if (g_error_count > 0) {
        print_errors();
        fprintf(stderr, "\n曜语编译器: 词法分析失败, 共 %d 个错误\n", g_error_count);
        free(lx.tokens);
        free(source);
        return 1;
    }

    /* 语法分析 */
    AstNode *prog = parse_program(lx.tokens, lx.token_count);

    if (only_ast) {
        print_ast(prog, 0);
        if (g_error_count > 0) {
            print_errors();
            return 1;
        }
        free(lx.tokens);
        free(source);
        return 0;
    }

    if (g_error_count > 0) {
        print_errors();
        fprintf(stderr, "\n曜语编译器: 语法分析失败, 共 %d 个错误\n", g_error_count);
        free(lx.tokens);
        free(source);
        return 1;
    }

    /* 语义分析 */
    types_init();
    sema_analyze(prog);

    if (g_error_count > 0) {
        print_errors();
        fprintf(stderr, "\n曜语编译器: 语义分析失败, 共 %d 个错误\n", g_error_count);
        free(lx.tokens);
        free(source);
        return 1;
    }

    /* 代码生成 */
    sb_init(&g_cgen);
    cgen_program(prog, &g_cgen);

    /* 简化: 直接在生成的C代码开头插入 setjmp 头 */
    {
        StrBuf final_code;
        sb_init(&final_code);
        sb_append(&final_code, "#include <setjmp.h>\nstatic jmp_buf _yao_main_jmp;\n\n");
        sb_append(&final_code, g_cgen.data);
        sb_free(&g_cgen);
        g_cgen = final_code;
    }

    if (only_stdout || only_c) {
        if (only_stdout) {
            printf("%s", g_cgen.data);
        } else {
            // -c: 写到 .c 文件
            char cfile[512];
            if (output_file) {
                strncpy(cfile, output_file, sizeof(cfile) - 1);
            } else {
                strncpy(cfile, input_file, sizeof(cfile) - 1);
                char *dot = strrchr(cfile, '.');
                if (dot) strcpy(dot, ".c");
                else strncat(cfile, ".c", sizeof(cfile) - strlen(cfile) - 1);
            }
            FILE *cf = fopen(cfile, "w");
            if (!cf) {
                fprintf(stderr, "曜语编译器: 无法写入 '%s': %s\n", cfile, strerror(errno));
                return 1;
            }
            fprintf(cf, "%s", g_cgen.data);
            fclose(cf);
            printf("曜语: 已生成 C 代码 → %s\n", cfile);
        }
        if (only_stdout || only_c) {
            sb_free(&g_cgen);
            free(lx.tokens);
            free(source);
            return 0;
        }
    }

    /* 编译C代码并链接 */
    char cfile[512];
    char ofile[512];
    snprintf(cfile, sizeof(cfile), "/tmp/yao_%d.c", (int)getpid());

    // 写临时C文件
    FILE *cf = fopen(cfile, "w");
    if (!cf) {
        fprintf(stderr, "曜语编译器: 无法创建临时文件\n");
        return 1;
    }
    fprintf(cf, "%s", g_cgen.data);
    fclose(cf);

    // 确定输出文件名
    if (output_file) {
        strncpy(ofile, output_file, sizeof(ofile) - 1);
    } else {
        strncpy(ofile, input_file, sizeof(ofile) - 1);
        char *dot = strrchr(ofile, '.');
        if (dot) *dot = '\0';
        // 默认输出到 a.out
        snprintf(ofile, sizeof(ofile), "./a.out");
    }

    // 检测生成代码中是否使用了 X11
    bool uses_x11 = (strstr(g_cgen.data, "_yao_x11_") != NULL);

    // 调用系统 C 编译器
    char cmd[4096];
    if (uses_x11) {
        snprintf(cmd, sizeof(cmd),
            "cc -O%d -DHAS_X11 -o '%s' '%s' -lm -lX11 2>&1",
            opt_level, ofile, cfile);
    } else {
        snprintf(cmd, sizeof(cmd),
            "cc -O%d -o '%s' '%s' -lm 2>&1",
            opt_level, ofile, cfile);
    }

    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        fprintf(stderr, "曜语编译器: 无法调用 C 编译器\n");
        unlink(cfile);
        return 1;
    }

    char buf[4096];
    bool c_error = false;
    while (fgets(buf, sizeof(buf), pipe)) {
        fprintf(stderr, "%s", buf);
        if (strstr(buf, "error:")) c_error = true;
    }
    int ret = pclose(pipe);

    unlink(cfile); // 删除临时C文件

    if (c_error || ret != 0) {
        if (only_stdout || only_c) {
            // 用户能看到C代码
            printf("%s", g_cgen.data);
        }
        fprintf(stderr, "\n曜语编译器: C 代码生成阶段发生错误\n");
        fprintf(stderr, "提示: 使用 -S 查看生成的 C 代码, 或使用 -c 保存 .c 文件\n");
        sb_free(&g_cgen);
        free(lx.tokens);
        free(source);
        return 1;
    }

    printf("曜语: 编译成功 → %s\n", ofile);

    sb_free(&g_cgen);
    free(lx.tokens);
    free(source);
    return 0;
}