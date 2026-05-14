#include "ecs_lexer.h"
#include "../ecs_fixed.h"
#include <string.h>
#include <ctype.h>

static const struct { const char* kw; uint8_t len; token_kind_t kind; } keywords[] = {
    {"prefab",       6,  TOK_KW_PREFAB},
    {"ability",      7,  TOK_KW_ABILITY},
    {"effect",       6,  TOK_KW_EFFECT},
    {"command",      7,  TOK_KW_COMMAND},
    {"entity",       6,  TOK_KW_TYPE_ENTITY},
    {"point",        5,  TOK_KW_TYPE_POINT},
    {"input",        5,  TOK_KW_INPUT},
    {"button",       6,  TOK_KW_BUTTON},
    {"stick",        5,  TOK_KW_STICK},
    {"tags",         4,  TOK_KW_TAGS},
    {"owned_tags",  10,  TOK_KW_OWNED_TAGS},
    {"attributes",  10,  TOK_KW_ATTRIBUTES},
    {"effects",      7,  TOK_KW_EFFECTS},
    {"abilities",    9,  TOK_KW_ABILITIES},
    {"requirements",12,  TOK_KW_REQUIREMENTS},
    {"ongoing",      7,  TOK_KW_ONGOING},
    {"cancel",       6,  TOK_KW_CANCEL},
    {"costs",        5,  TOK_KW_COSTS},
    {"cooldowns",    9,  TOK_KW_COOLDOWNS},
    {"duration",     8,  TOK_KW_DURATION},
    {"period",       6,  TOK_KW_PERIOD},
    {"every",        5,  TOK_KW_EVERY},
    {"source_entity", 13, TOK_KW_SOURCE},
    {"target_entity", 13, TOK_KW_TARGET},
    {"on",           2,  TOK_KW_ON},
    {"all",          3,  TOK_KW_ALL},
    {"any",          3,  TOK_KW_ANY},
    {"none",         4,  TOK_KW_NONE},
    {"exact",        5,  TOK_KW_EXACT},
    {"if",           2,  TOK_KW_IF},
    {"else",         4,  TOK_KW_ELSE},
    {"while",        5,  TOK_KW_WHILE},
    {"wait",         4,  TOK_KW_WAIT},
    {"wait_event",  10,  TOK_KW_WAIT_EVENT},
    {"timeout",      7,  TOK_KW_TIMEOUT},
    {"emit",         4,  TOK_KW_EMIT},
    {"apply",        5,  TOK_KW_APPLY},
    {"remove",       6,  TOK_KW_REMOVE},
    {"despawn",      7,  TOK_KW_DESPAWN},
    {"spawn",        5,  TOK_KW_SPAWN},
    {"return",       6,  TOK_KW_RETURN},
    {"break",        5,  TOK_KW_BREAK},
    {"continue",     8,  TOK_KW_CONTINUE},
    {"and",          3,  TOK_KW_AND},
    {"or",           2,  TOK_KW_OR},
    {"not",          3,  TOK_KW_NOT},
    {"self",         4,  TOK_KW_SELF},
    {"event",        5,  TOK_KW_EVENT},
    {"locals",       6,  TOK_KW_LOCALS},
    {"stacks",       6,  TOK_KW_STACKS},
    {"now",          3,  TOK_KW_NOW},
    {"timed_out",    9,  TOK_KW_TIMED_OUT},
    {"disabled",     8,  TOK_KW_DISABLED},
    {"infinite",     8,  TOK_KW_INFINITE},
};
static const uint32_t keyword_count = (uint32_t)(sizeof(keywords) / sizeof(keywords[0]));

static int is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_ident_cont (int c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static int is_digit      (int c) { return c >= '0' && c <= '9'; }

static int peek_at(const lexer_t* l, uint32_t off) {
    return (l->pos + off < l->len) ? (unsigned char)l->src[l->pos + off] : 0;
}
static int peek_ch(const lexer_t* l) { return peek_at(l, 0); }

static void advance(lexer_t* l) {
    if (l->pos < l->len) {
        if (l->src[l->pos] == '\n') l->line++;
        l->pos++;
    }
}

static void skip_ws_and_comments(lexer_t* l) {
    for (;;) {
        int c = peek_ch(l);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(l); continue; }
        if (c == '/' && peek_at(l, 1) == '/') {
            while (l->pos < l->len && l->src[l->pos] != '\n') l->pos++;
            continue;
        }
        if (c == '/' && peek_at(l, 1) == '*') {
            advance(l); advance(l);
            while (l->pos < l->len && !(l->src[l->pos] == '*' && peek_at(l, 1) == '/')) advance(l);
            if (l->pos < l->len) { advance(l); advance(l); }
            continue;
        }
        return;
    }
}

static token_kind_t kw_lookup(const char* s, uint32_t len) {
    for (uint32_t i = 0; i < keyword_count; i++) {
        if (keywords[i].len == len && memcmp(keywords[i].kw, s, len) == 0) {
            return keywords[i].kind;
        }
    }
    return TOK_IDENT;
}

static token_t make_tok(lexer_t* l, token_kind_t k, const char* start, uint32_t len) {
    token_t t;
    t.kind  = k;
    t.start = start;
    t.len   = len;
    t.line  = l->line;
    t.u.int_val = 0;
    return t;
}

static token_t scan_ident_or_kw(lexer_t* l) {
    const char* start = l->src + l->pos;
    uint32_t start_line = l->line;
    while (l->pos < l->len) {
        int c = peek_ch(l);
        if (is_ident_cont(c) || c == '.') advance(l);
        else break;
    }
    uint32_t len = (uint32_t)(l->src + l->pos - start);
    token_kind_t k = TOK_IDENT;
    /* Only single-segment idents may be keywords (no dots). */
    int has_dot = 0;
    for (uint32_t i = 0; i < len; i++) if (start[i] == '.') { has_dot = 1; break; }
    if (!has_dot) k = kw_lookup(start, len);
    token_t t;
    t.kind  = k;
    t.start = start;
    t.len   = len;
    t.line  = start_line;
    t.u.int_val = 0;
    return t;
}

/* Number forms: 123, 1.5, 30%, 5s, 30t.
   Integer: plain digits, no suffix → TOK_INT.
   Fixed:   has '.' or '%' or 's' or 't' suffix → TOK_FIXED (Q16.16). */
static token_t scan_number(lexer_t* l) {
    const char* start      = l->src + l->pos;
    uint32_t    start_line = l->line;

    int32_t whole = 0;
    while (is_digit(peek_ch(l))) {
        whole = whole * 10 + (peek_ch(l) - '0');
        advance(l);
    }

    int   is_fixed = 0;
    int32_t fixed_val = fixed_from_int(whole);

    if (peek_ch(l) == '.' && is_digit(peek_at(l, 1))) {
        advance(l); /* consume '.' */
        uint32_t frac = 0;
        while (is_digit(peek_ch(l))) {
            frac = frac * 10 + (uint32_t)(peek_ch(l) - '0');
            advance(l);
        }
        fixed_val = fixed_from_parts(whole, frac);
        is_fixed  = 1;
    }

    int suffix = peek_ch(l);
    if (suffix == '%') {
        advance(l);
        /* 30% → 0.30 */
        fixed_val = fixed_div(fixed_val, fixed_from_int(100));
        is_fixed  = 1;
    } else if (suffix == 's') {
        advance(l);
        /* seconds — caller decides tick-rate scaling at codegen; store as fixed seconds. */
        if (!is_fixed) fixed_val = fixed_from_int(whole);
        is_fixed = 1;
    } else if (suffix == 't') {
        advance(l);
        /* ticks — integer count, but represent as fixed for uniformity. */
        if (!is_fixed) fixed_val = fixed_from_int(whole);
        is_fixed = 1;
    }

    uint32_t len = (uint32_t)(l->src + l->pos - start);
    token_t t;
    t.start = start;
    t.len   = len;
    t.line  = start_line;
    if (is_fixed) {
        t.kind         = TOK_FIXED;
        t.u.fixed_val  = fixed_val;
    } else {
        t.kind        = TOK_INT;
        t.u.int_val   = whole;
    }
    return t;
}

static token_t scan_punct(lexer_t* l) {
    const char* start      = l->src + l->pos;
    uint32_t    start_line = l->line;
    int c  = peek_ch(l);
    int c2 = peek_at(l, 1);
    token_kind_t k = TOK_ERROR;
    uint32_t len = 1;

    switch (c) {
        case '{': k = TOK_LBRACE;    break;
        case '}': k = TOK_RBRACE;    break;
        case '(': k = TOK_LPAREN;    break;
        case ')': k = TOK_RPAREN;    break;
        case '[': k = TOK_LBRACKET;  break;
        case ']': k = TOK_RBRACKET;  break;
        case ',': k = TOK_COMMA;     break;
        case ';': k = TOK_SEMICOLON; break;
        case '.': k = TOK_DOT;       break;
        case '+': if (c2 == '=') { k = TOK_PLUS_EQ;  len = 2; } else k = TOK_PLUS;  break;
        case '-': if (c2 == '=') { k = TOK_MINUS_EQ; len = 2; }
                  else if (c2 == '>') { k = TOK_ARROW; len = 2; }
                  else k = TOK_MINUS; break;
        case '*': if (c2 == '=') { k = TOK_STAR_EQ;  len = 2; } else k = TOK_STAR;  break;
        case '/': if (c2 == '=') { k = TOK_SLASH_EQ; len = 2; } else k = TOK_SLASH; break;
        case '=': if (c2 == '=') { k = TOK_EQ;       len = 2; } else k = TOK_ASSIGN; break;
        case '!': if (c2 == '=') { k = TOK_NEQ;      len = 2; } else k = TOK_ERROR;  break;
        case '<': if (c2 == '=') { k = TOK_LE;       len = 2; } else k = TOK_LT;     break;
        case '>': if (c2 == '=') { k = TOK_GE;       len = 2; } else k = TOK_GT;     break;
        default:  k = TOK_ERROR; break;
    }
    for (uint32_t i = 0; i < len; i++) advance(l);
    token_t t;
    t.kind  = k;
    t.start = start;
    t.len   = len;
    t.line  = start_line;
    t.u.int_val = 0;
    return t;
}

void lexer_init(lexer_t* l, const char* src, uint32_t len, scan_mode_t mode) {
    l->src     = src;
    l->pos     = 0;
    l->len     = len;
    l->line    = 1;
    l->mode    = mode;
    l->current = (token_t){0};
    /* Prime current with the first token. lexer_next returns OLD current and
       scans into l->current; we discard the return value. */
    (void)lexer_next(l);
}

token_t lexer_peek(const lexer_t* l) { return l->current; }

token_t lexer_next(lexer_t* l) {
    token_t out = l->current;

    for (;;) {
        skip_ws_and_comments(l);
        if (l->pos >= l->len) { l->current = make_tok(l, TOK_EOF, l->src + l->pos, 0); break; }

        int c = peek_ch(l);
        if (is_ident_start(c))      l->current = scan_ident_or_kw(l);
        else if (is_digit(c))       l->current = scan_number(l);
        else                        l->current = scan_punct(l);

        /* SCAN_LIGHT: skip body interiors of script-style blocks (only meaningful
           after seeing `on`/`every`/`requirements`/...). For now LIGHT == FULL —
           consumer filters by position. */
        break;
    }
    return out;
}
