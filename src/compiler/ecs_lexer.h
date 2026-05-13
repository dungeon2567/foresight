#pragma once

#include <stdint.h>
#include <stddef.h>

/* ==========================================================================
   Lexer — two modes:
     SCAN_LIGHT (pass 1): yields only tag-position tokens; skips bodies.
     SCAN_FULL  (pass 4): yields every token for recursive-descent parser.
   ========================================================================== */

typedef enum {
    /* Literals */
    TOK_IDENT,      /* [a-z][a-z0-9_]* or dotted tag path */
    TOK_INT,        /* 123 */
    TOK_FIXED,      /* 1.5, 30%, 5s, 30t */

    /* Punctuation */
    TOK_LBRACE,     /* { */
    TOK_RBRACE,     /* } */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_LBRACKET,   /* [ */
    TOK_RBRACKET,   /* ] */
    TOK_COMMA,
    TOK_SEMICOLON,
    TOK_DOT,
    TOK_ASSIGN,     /* = */
    TOK_PLUS_EQ,    /* += */
    TOK_MINUS_EQ,   /* -= */
    TOK_STAR_EQ,    /* *= */
    TOK_SLASH_EQ,   /* /= */
    TOK_ARROW,      /* -> */
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_EQ,         /* == */
    TOK_NEQ,        /* != */

    /* Keywords */
    TOK_KW_PREFAB, TOK_KW_ABILITY, TOK_KW_EFFECT,
    TOK_KW_TAGS, TOK_KW_OWNED_TAGS, TOK_KW_ATTRIBUTES,
    TOK_KW_EFFECTS, TOK_KW_ABILITIES,
    TOK_KW_REQUIREMENTS, TOK_KW_ONGOING, TOK_KW_CANCEL,
    TOK_KW_COSTS, TOK_KW_COOLDOWN, TOK_KW_DURATION, TOK_KW_PERIOD,
    TOK_KW_EVERY, TOK_KW_SOURCE, TOK_KW_TARGET,
    TOK_KW_ON, TOK_KW_ALL, TOK_KW_ANY, TOK_KW_NONE, TOK_KW_EXACT,
    TOK_KW_IF, TOK_KW_ELSE, TOK_KW_WHILE,
    TOK_KW_WAIT, TOK_KW_WAIT_EVENT, TOK_KW_TIMEOUT,
    TOK_KW_EMIT, TOK_KW_APPLY, TOK_KW_REMOVE,
    TOK_KW_DESPAWN, TOK_KW_SPAWN,
    TOK_KW_RETURN, TOK_KW_BREAK, TOK_KW_CONTINUE,
    TOK_KW_AND, TOK_KW_OR, TOK_KW_NOT,
    TOK_KW_SELF, TOK_KW_EVENT, TOK_KW_LOCALS,
    TOK_KW_STACKS, TOK_KW_NOW, TOK_KW_TIMED_OUT, TOK_KW_DISABLED,
    TOK_KW_INFINITE,

    TOK_EOF,
    TOK_ERROR,
} token_kind_t;

typedef enum {
    SCAN_LIGHT = 0,   /* pass 1: tag positions only */
    SCAN_FULL  = 1,   /* pass 4: every token */
} scan_mode_t;

typedef struct {
    token_kind_t kind;
    const char*  start;    /* pointer into source */
    uint32_t     len;
    uint32_t     line;
    union {
        int32_t  int_val;
        int32_t  fixed_val;   /* Q16.16 */
    } u;
} token_t;

typedef struct {
    const char* src;
    uint32_t    pos;
    uint32_t    len;
    uint32_t    line;
    scan_mode_t mode;
    token_t     current;
} lexer_t;

void    lexer_init(lexer_t* l, const char* src, uint32_t len, scan_mode_t mode);
token_t lexer_next(lexer_t* l);
token_t lexer_peek(const lexer_t* l);
