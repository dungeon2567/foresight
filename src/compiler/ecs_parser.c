#include "ecs_parser.h"
#include "../ecs_common.h"
#include "../ecs_script_query.h"   /* subject_t (SUBJ_SELF / TARGET / SOURCE) */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 8-bit FNV-1a — bucket key only, never stored. */
static inline uint8_t tag_bucket_of(const char* s, uint32_t len) {
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < len; i++) { h ^= (uint8_t)s[i]; h *= 0x01000193u; }
    return (uint8_t)(h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24));
}

/* Intern a tag string by (str, len) equality. Returns the index — stored in
   AST node u.tag.tag_idx / u.attr.tag_idx. Bucket-chained: O(1) amortized
   even for thousands of refs (vs O(N^2) for linear scan). */
static uint32_t intern_tag(parser_t* p, const char* s, uint32_t len) {
    uint8_t bk = tag_bucket_of(s, len);
    for (uint32_t i = p->tag_strs_head[bk]; i; i = p->tag_strs_next[i - 1]) {
        const tag_str_t* t = &p->tag_strs[i - 1];
        if (t->len == len && memcmp(t->str, s, len) == 0) return i - 1;
    }
    if (p->tag_strs_count >= p->tag_strs_cap) {
        uint32_t cap = p->tag_strs_cap ? p->tag_strs_cap * 2 : 64;
        p->tag_strs      = (tag_str_t*)ecs_xrealloc(p->tag_strs,      cap * sizeof(tag_str_t));
        p->tag_strs_next = (uint32_t*) ecs_xrealloc(p->tag_strs_next, cap * sizeof(uint32_t));
        p->tag_strs_cap  = cap;
    }
    uint32_t idx = p->tag_strs_count++;
    p->tag_strs[idx]      = (tag_str_t){ s, len };
    p->tag_strs_next[idx] = p->tag_strs_head[bk];
    p->tag_strs_head[bk]  = idx + 1;
    return idx;
}

/* ==========================================================================
   Arena management.
   ========================================================================== */
static void arena_reserve(ast_arena_t* a, uint32_t n) {
    if (a->count + n <= a->cap) return;
    uint32_t cap = a->cap ? a->cap : 64;
    while (cap < a->count + n) cap *= 2;
    a->nodes = (ast_node_t*)ecs_xrealloc(a->nodes, cap * sizeof(ast_node_t));
    a->cap = cap;
}

static void run_reserve(ast_arena_t* a, uint32_t n) {
    if (a->child_runs_count + n <= a->child_runs_cap) return;
    uint32_t cap = a->child_runs_cap ? a->child_runs_cap : 64;
    while (cap < a->child_runs_count + n) cap *= 2;
    a->child_runs = (ast_idx_t*)ecs_xrealloc(a->child_runs, cap * sizeof(ast_idx_t));
    a->child_runs_cap = cap;
}

static ast_idx_t arena_alloc(ast_arena_t* a, ast_kind_t kind) {
    arena_reserve(a, 1);
    /* Slot 0 reserved for null. */
    if (a->count == 0) {
        memset(&a->nodes[0], 0, sizeof(ast_node_t));
        a->count = 1;
    }
    ast_idx_t idx = a->count++;
    ast_node_t* n = &a->nodes[idx];
    memset(n, 0, sizeof(*n));
    n->kind = (uint16_t)kind;
    return idx;
}

static void set_children(ast_arena_t* a, ast_idx_t idx, const ast_idx_t* kids, uint32_t n) {
    ast_node_t* node = &a->nodes[idx];
    node->n_children = (uint8_t)(n > 255 ? 255 : n);
    if (n <= 4) {
        for (uint32_t i = 0; i < n; i++) node->children[i] = kids[i];
        for (uint32_t i = n; i < 4; i++) node->children[i] = 0;
    } else {
        run_reserve(a, n);
        ast_idx_t base = a->child_runs_count;
        for (uint32_t i = 0; i < n; i++) a->child_runs[base + i] = kids[i];
        a->child_runs_count += n;
        node->children[0] = base;
        node->children[1] = node->children[2] = node->children[3] = 0;
        node->n_children  = (uint8_t)(n > 255 ? 255 : n);
        /* count overflow flag: encode in flags if needed; >4 implicitly means overflow. */
    }
}

/* ==========================================================================
   Token helpers.
   ========================================================================== */
typedef struct {
    parser_t*  p;
    ast_idx_t* kids_buf;
    uint32_t   kids_cap;
    uint32_t   kids_count;
} parse_ctx_t;

static void error(parser_t* p, const char* msg) {
    fprintf(stderr, "%s:%u: error: %s\n", p->filename ? p->filename : "<src>",
            (unsigned)p->lex.current.line, msg);
    p->had_error = 1;
}

static token_t peek(parser_t* p) { return p->lex.current; }

/* 2-token lookahead: scan one token then rewind lexer state. */
static token_t peek2(parser_t* p) {
    uint32_t pos  = p->lex.pos;
    uint32_t line = p->lex.line;
    token_t  cur  = p->lex.current;
    lexer_next(&p->lex);
    token_t t = p->lex.current;
    p->lex.pos     = pos;
    p->lex.line    = line;
    p->lex.current = cur;
    return t;
}

static int check(parser_t* p, token_kind_t k) { return peek(p).kind == k; }

static int match(parser_t* p, token_kind_t k) {
    if (peek(p).kind != k) return 0;
    lexer_next(&p->lex);
    return 1;
}

static token_t consume(parser_t* p) {
    return lexer_next(&p->lex);
}

static token_t expect(parser_t* p, token_kind_t k, const char* what) {
    token_t t = peek(p);
    if (t.kind != k) {
        error(p, what);
        return t;
    }
    lexer_next(&p->lex);
    return t;
}

/* Accept TOK_IDENT or any reserved keyword as a name token — uses the
   token's source bytes (start, len) regardless of kind. Lets things like
   `target` / `source` / `self` / `position` be used as user-defined
   field / slot names even though the lexer classifies them as keywords. */
static token_t expect_name(parser_t* p, const char* what) {
    token_t t = peek(p);
    /* Reject anything that isn't an alphabetic identifier-or-keyword. */
    int is_name =
        (t.kind == TOK_IDENT) ||
        (t.kind >= TOK_KW_PREFAB && t.kind < TOK_EOF);
    if (!is_name) {
        error(p, what);
        return t;
    }
    lexer_next(&p->lex);
    return t;
}

/* ==========================================================================
   Kids buffer — reusable to avoid alloc per node.
   ========================================================================== */
static void kids_clear(parse_ctx_t* c) { c->kids_count = 0; }
static void kids_push(parse_ctx_t* c, ast_idx_t idx) {
    if (c->kids_count >= c->kids_cap) {
        uint32_t cap = c->kids_cap ? c->kids_cap * 2 : 16;
        c->kids_buf = (ast_idx_t*)ecs_xrealloc(c->kids_buf, cap * sizeof(ast_idx_t));
        c->kids_cap = cap;
    }
    c->kids_buf[c->kids_count++] = idx;
}

/* ==========================================================================
   Forward decls.
   ========================================================================== */
static ast_idx_t parse_decl     (parse_ctx_t* c);
static ast_idx_t parse_prefab   (parse_ctx_t* c, uint32_t name_idx);
static ast_idx_t parse_ability  (parse_ctx_t* c, uint32_t name_idx);
static ast_idx_t parse_effect   (parse_ctx_t* c, uint32_t name_idx);
static ast_idx_t parse_block_body(parse_ctx_t* c, token_kind_t end);
static ast_idx_t parse_query    (parse_ctx_t* c);
static ast_idx_t parse_query_section(parse_ctx_t* c, ast_kind_t kind);
static ast_idx_t parse_expr     (parse_ctx_t* c);
static ast_idx_t parse_stmt     (parse_ctx_t* c);
static ast_idx_t parse_tag_path (parse_ctx_t* c, uint32_t* out_idx);

/* ==========================================================================
   Tag path: dotted identifier sequence. Lexer emits the full path as one
   TOK_IDENT (dots allowed in idents); just hash it.
   ========================================================================== */
static ast_idx_t parse_tag_path(parse_ctx_t* c, uint32_t* out_idx) {
    token_t t = peek(c->p);
    if (t.kind != TOK_IDENT) { error(c->p, "expected tag path"); return 0; }
    lexer_next(&c->p->lex);
    uint32_t h = intern_tag(c->p,t.start, t.len);
    if (out_idx) *out_idx = h;
    ast_idx_t n = arena_alloc(&c->p->arena, AST_CLAUSE_TAG);
    c->p->arena.nodes[n].u.tag.tag_idx = h;
    return n;
}

/* ==========================================================================
   Subject keyword → ast_subject_t. Returns -1 if not a subject.
   ========================================================================== */
static int parse_subject_kw(parser_t* p) {
    switch (peek(p).kind) {
        case TOK_KW_SELF:   lexer_next(&p->lex); return SUBJ_SELF;
        case TOK_KW_TARGET: lexer_next(&p->lex); return SUBJ_TARGET;
        case TOK_KW_SOURCE: lexer_next(&p->lex); return SUBJ_SOURCE;
        default: return -1;
    }
}

/* ==========================================================================
   Primary expr: literal, ident-path (attr/tag/builtin), parenthesized.
   ========================================================================== */
static ast_idx_t parse_primary(parse_ctx_t* c) {
    token_t t = peek(c->p);
    if (t.kind == TOK_INT) {
        lexer_next(&c->p->lex);
        ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_LIT);
        c->p->arena.nodes[n].u.lit = t.u.int_val;
        return n;
    }
    if (t.kind == TOK_FIXED) {
        lexer_next(&c->p->lex);
        ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_LIT);
        c->p->arena.nodes[n].u.lit = t.u.fixed_val;
        c->p->arena.nodes[n].flags = 1; /* flag: is_fixed */
        return n;
    }
    if (t.kind == TOK_LPAREN) {
        lexer_next(&c->p->lex);
        ast_idx_t e = parse_expr(c);
        if (check(c->p, TOK_COMMA)) {
            /* Tuple literal: `(a, b)` or `(a, b, c)`. Used for vec3 / quat field
               init on component blocks. Trailing comma allowed. */
            uint32_t saved = c->kids_count;
            kids_push(c, e);
            while (match(c->p, TOK_COMMA)) {
                if (check(c->p, TOK_RPAREN)) break;
                kids_push(c, parse_expr(c));
            }
            expect(c->p, TOK_RPAREN, "expected ')'");
            ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_TUPLE);
            set_children(&c->p->arena, n, &c->kids_buf[saved], c->kids_count - saved);
            c->kids_count = saved;
            return n;
        }
        expect(c->p, TOK_RPAREN, "expected ')'");
        return e;
    }
    if (t.kind == TOK_MINUS) {
        lexer_next(&c->p->lex);
        ast_idx_t sub = parse_primary(c);
        ast_idx_t n   = arena_alloc(&c->p->arena, AST_EXPR_UNOP);
        c->p->arena.nodes[n].u.unop.op = UNOP_NEG;
        ast_idx_t kids[1] = { sub };
        set_children(&c->p->arena, n, kids, 1);
        return n;
    }
    if (t.kind == TOK_KW_NOT) {
        lexer_next(&c->p->lex);
        ast_idx_t sub = parse_primary(c);
        ast_idx_t n   = arena_alloc(&c->p->arena, AST_EXPR_UNOP);
        c->p->arena.nodes[n].u.unop.op = UNOP_NOT;
        ast_idx_t kids[1] = { sub };
        set_children(&c->p->arena, n, kids, 1);
        return n;
    }
    /* Well-known scalars. */
    if (t.kind == TOK_KW_STACKS || t.kind == TOK_KW_NOW ||
        t.kind == TOK_KW_TIMED_OUT || t.kind == TOK_KW_DISABLED) {
        lexer_next(&c->p->lex);
        ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_WELL_KNOWN);
        c->p->arena.nodes[n].u.wk.well_known =
            (uint8_t)(t.kind == TOK_KW_STACKS  ? 0 :
                      t.kind == TOK_KW_NOW     ? 1 :
                      t.kind == TOK_KW_TIMED_OUT ? 2 : 3);
        return n;
    }
    /* event.field — payload access. */
    if (t.kind == TOK_KW_EVENT) {
        lexer_next(&c->p->lex);
        expect(c->p, TOK_DOT, "expected '.' after 'event'");
        uint32_t h = 0;
        token_t name = expect_name(c->p, "expected payload field");
        h = intern_tag(c->p,name.start, name.len);
        ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_PAYLOAD);
        c->p->arena.nodes[n].u.tag.tag_idx = h;
        return n;
    }
    /* locals[N] */
    if (t.kind == TOK_KW_LOCALS) {
        lexer_next(&c->p->lex);
        expect(c->p, TOK_LBRACKET, "expected '['");
        token_t idx = expect(c->p, TOK_INT, "expected local index");
        expect(c->p, TOK_RBRACKET, "expected ']'");
        ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_LOCAL);
        c->p->arena.nodes[n].u.local.local_idx = (uint8_t)idx.u.int_val;
        return n;
    }
    /* subject.attr — self.hp / target.hp / source.hp */
    if (t.kind == TOK_KW_SELF || t.kind == TOK_KW_TARGET || t.kind == TOK_KW_SOURCE) {
        int subj = parse_subject_kw(c->p);
        expect(c->p, TOK_DOT, "expected '.' after subject");
        token_t name = expect_name(c->p, "expected attr path");
        ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_ATTR);
        c->p->arena.nodes[n].u.attr.subj     = (ast_subject_t)subj;
        c->p->arena.nodes[n].u.attr.tag_idx = intern_tag(c->p,name.start, name.len);
        return n;
    }
    /* Builtin function call: ident(args...) */
    if (t.kind == TOK_IDENT) {
        lexer_next(&c->p->lex);
        if (check(c->p, TOK_LPAREN)) {
            /* function call — match name to builtin */
            builtin_t bid = BUILTIN_CLAMP;
            int matched = 0;
            #define MATCH_BI(name, id) if (t.len == sizeof(name)-1 && memcmp(t.start, name, t.len) == 0) { bid = id; matched = 1; }
            MATCH_BI("clamp",         BUILTIN_CLAMP)
            MATCH_BI("min",           BUILTIN_MIN)
            MATCH_BI("max",           BUILTIN_MAX)
            MATCH_BI("abs",           BUILTIN_ABS)
            MATCH_BI("distance",      BUILTIN_DISTANCE)
            MATCH_BI("is_behind",     BUILTIN_IS_BEHIND)
            MATCH_BI("is_in_front",   BUILTIN_IS_IN_FRONT)
            MATCH_BI("angle_to",      BUILTIN_ANGLE_TO)
            MATCH_BI("random_range",  BUILTIN_RANDOM_RANGE)
            MATCH_BI("cooldown_remaining", BUILTIN_COOLDOWN_REMAINING)
            MATCH_BI("has_tag",       BUILTIN_HAS_TAG)
            MATCH_BI("match",         BUILTIN_MATCH)
            #undef MATCH_BI
            if (!matched) { error(c->p, "unknown builtin"); }
            lexer_next(&c->p->lex); /* consume '(' */
            ast_idx_t argk[4]; uint32_t nargs = 0;
            if (!check(c->p, TOK_RPAREN)) {
                do {
                    if (nargs < 4) argk[nargs++] = parse_expr(c);
                    else { (void)parse_expr(c); }
                } while (match(c->p, TOK_COMMA));
            }
            expect(c->p, TOK_RPAREN, "expected ')'");
            ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_BUILTIN);
            c->p->arena.nodes[n].u.builtin.id = bid;
            set_children(&c->p->arena, n, argk, nargs);
            return n;
        }
        /* Plain ident in expr position: treat as tag reference for clauses or
           as an attr on implicit self — defer to caller via tag-hash node. */
        ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_ATTR);
        c->p->arena.nodes[n].u.attr.subj     = SUBJ_SELF;
        c->p->arena.nodes[n].u.attr.tag_idx = intern_tag(c->p,t.start, t.len);
        return n;
    }
    error(c->p, "expected expression");
    consume(c->p);
    return 0;
}

/* Precedence climb. */
static int  binop_prec(token_kind_t k) {
    switch (k) {
        case TOK_KW_OR:  return 1;
        case TOK_KW_AND: return 2;
        case TOK_EQ: case TOK_NEQ: case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE: return 3;
        case TOK_PLUS: case TOK_MINUS: return 4;
        case TOK_STAR: case TOK_SLASH: return 5;
        default: return 0;
    }
}
static binop_t binop_of(token_kind_t k) {
    switch (k) {
        case TOK_KW_OR:  return BINOP_OR;
        case TOK_KW_AND: return BINOP_AND;
        case TOK_EQ:     return BINOP_EQ;
        case TOK_NEQ:    return BINOP_NE;
        case TOK_LT:     return BINOP_LT;
        case TOK_LE:     return BINOP_LE;
        case TOK_GT:     return BINOP_GT;
        case TOK_GE:     return BINOP_GE;
        case TOK_PLUS:   return BINOP_ADD;
        case TOK_MINUS:  return BINOP_SUB;
        case TOK_STAR:   return BINOP_MUL;
        case TOK_SLASH:  return BINOP_DIV;
        default:         return BINOP_ADD;
    }
}

static ast_idx_t parse_expr_prec(parse_ctx_t* c, int min_prec) {
    ast_idx_t lhs = parse_primary(c);
    for (;;) {
        token_kind_t k = peek(c->p).kind;
        int prec = binop_prec(k);
        if (prec < min_prec) break;
        lexer_next(&c->p->lex);
        ast_idx_t rhs = parse_expr_prec(c, prec + 1);
        ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_BINOP);
        c->p->arena.nodes[n].u.binop.op = binop_of(k);
        ast_idx_t kids[2] = { lhs, rhs };
        set_children(&c->p->arena, n, kids, 2);
        lhs = n;
    }
    return lhs;
}

static ast_idx_t parse_expr(parse_ctx_t* c) { return parse_expr_prec(c, 1); }

/* ==========================================================================
   Statement parsing for script bodies (inside `on { ... }` / `every { ... }`).
   ========================================================================== */
static ast_idx_t parse_block_stmts(parse_ctx_t* c) {
    expect(c->p, TOK_LBRACE, "expected '{'");
    uint32_t saved = c->kids_count;
    while (!check(c->p, TOK_RBRACE) && !check(c->p, TOK_EOF)) {
        ast_idx_t s = parse_stmt(c);
        kids_push(c, s);
        match(c->p, TOK_SEMICOLON);
    }
    expect(c->p, TOK_RBRACE, "expected '}'");
    ast_idx_t blk = arena_alloc(&c->p->arena, AST_EXPR_BLOCK);
    set_children(&c->p->arena, blk, &c->kids_buf[saved], c->kids_count - saved);
    c->kids_count = saved;
    return blk;
}

static ast_idx_t parse_stmt(parse_ctx_t* c) {
    token_kind_t k = peek(c->p).kind;
    switch (k) {
        case TOK_KW_IF: {
            lexer_next(&c->p->lex);
            expect(c->p, TOK_LPAREN, "expected '('");
            ast_idx_t cond = parse_expr(c);
            expect(c->p, TOK_RPAREN, "expected ')'");
            ast_idx_t then_b = parse_block_stmts(c);
            ast_idx_t else_b = 0;
            if (match(c->p, TOK_KW_ELSE)) {
                if (check(c->p, TOK_KW_IF)) else_b = parse_stmt(c);
                else                        else_b = parse_block_stmts(c);
            }
            ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_IF);
            ast_idx_t kids[3] = { cond, then_b, else_b };
            set_children(&c->p->arena, n, kids, else_b ? 3 : 2);
            return n;
        }
        case TOK_KW_WHILE: {
            lexer_next(&c->p->lex);
            expect(c->p, TOK_LPAREN, "expected '('");
            ast_idx_t cond = parse_expr(c);
            expect(c->p, TOK_RPAREN, "expected ')'");
            ast_idx_t body = parse_block_stmts(c);
            ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_WHILE);
            ast_idx_t kids[2] = { cond, body };
            set_children(&c->p->arena, n, kids, 2);
            return n;
        }
        case TOK_KW_WAIT: {
            lexer_next(&c->p->lex);
            ast_idx_t arg = parse_expr(c);
            ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_WAIT);
            ast_idx_t kids[1] = { arg };
            set_children(&c->p->arena, n, kids, 1);
            return n;
        }
        case TOK_KW_WAIT_EVENT: {
            lexer_next(&c->p->lex);
            uint32_t h = 0;
            (void)parse_tag_path(c, &h);
            ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_WAIT_EVENT);
            c->p->arena.nodes[n].u.tag.tag_idx = h;
            ast_idx_t timeout = 0;
            if (match(c->p, TOK_KW_TIMEOUT)) timeout = parse_expr(c);
            ast_idx_t kids[1] = { timeout };
            set_children(&c->p->arena, n, kids, timeout ? 1 : 0);
            return n;
        }
        case TOK_KW_RETURN: {
            lexer_next(&c->p->lex);
            ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_RETURN);
            if (!check(c->p, TOK_SEMICOLON) && !check(c->p, TOK_RBRACE)) {
                ast_idx_t e = parse_expr(c);
                ast_idx_t kids[1] = { e };
                set_children(&c->p->arena, n, kids, 1);
            }
            return n;
        }
        case TOK_KW_BREAK:    lexer_next(&c->p->lex); return arena_alloc(&c->p->arena, AST_EXPR_BREAK);
        case TOK_KW_CONTINUE: lexer_next(&c->p->lex); return arena_alloc(&c->p->arena, AST_EXPR_CONTINUE);
        case TOK_KW_EMIT: {
            lexer_next(&c->p->lex);
            uint32_t h = 0;
            (void)parse_tag_path(c, &h);
            ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_EMIT);
            c->p->arena.nodes[n].u.tag.tag_idx = h;
            /* Optional payload: (k1 = v1, k2 = v2). */
            if (match(c->p, TOK_LPAREN)) {
                uint32_t saved = c->kids_count;
                if (!check(c->p, TOK_RPAREN)) {
                    do {
                        token_t name = expect_name(c->p, "expected payload field");
                        expect(c->p, TOK_ASSIGN, "expected '='");
                        ast_idx_t v = parse_expr(c);
                        ast_idx_t fk = arena_alloc(&c->p->arena, AST_EXPR_ASSIGN);
                        c->p->arena.nodes[fk].u.tag.tag_idx = intern_tag(c->p,name.start, name.len);
                        ast_idx_t kk[1] = { v };
                        set_children(&c->p->arena, fk, kk, 1);
                        kids_push(c, fk);
                    } while (match(c->p, TOK_COMMA));
                }
                expect(c->p, TOK_RPAREN, "expected ')'");
                set_children(&c->p->arena, n, &c->kids_buf[saved], c->kids_count - saved);
                c->kids_count = saved;
            }
            return n;
        }
        case TOK_KW_APPLY: {
            lexer_next(&c->p->lex);
            uint32_t h = 0;
            (void)parse_tag_path(c, &h);
            ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_APPLY);
            c->p->arena.nodes[n].u.tag.tag_idx = h;
            return n;
        }
        case TOK_KW_REMOVE: {
            lexer_next(&c->p->lex);
            uint32_t h = 0;
            (void)parse_tag_path(c, &h);
            ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_REMOVE);
            c->p->arena.nodes[n].u.tag.tag_idx = h;
            return n;
        }
        case TOK_KW_CANCEL: {
            lexer_next(&c->p->lex);
            uint32_t h = 0;
            (void)parse_tag_path(c, &h);
            ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_CANCEL);
            c->p->arena.nodes[n].u.tag.tag_idx = h;
            return n;
        }
        case TOK_KW_DESPAWN: lexer_next(&c->p->lex); return arena_alloc(&c->p->arena, AST_EXPR_DESPAWN);
        case TOK_KW_SPAWN: {
            lexer_next(&c->p->lex);
            uint32_t h = 0;
            (void)parse_tag_path(c, &h);
            ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_SPAWN);
            c->p->arena.nodes[n].u.tag.tag_idx = h;
            return n;
        }
        case TOK_LBRACE: return parse_block_stmts(c);
        default: {
            /* Assignment or bare expr: parse lhs, then maybe assign-op. */
            ast_idx_t lhs = parse_expr(c);
            token_kind_t a = peek(c->p).kind;
            if (a == TOK_ASSIGN || a == TOK_PLUS_EQ || a == TOK_MINUS_EQ ||
                a == TOK_STAR_EQ || a == TOK_SLASH_EQ) {
                lexer_next(&c->p->lex);
                ast_idx_t rhs = parse_expr(c);
                /* Encode +=/-= as assign of (lhs op rhs). */
                if (a != TOK_ASSIGN) {
                    binop_t bop = (a == TOK_PLUS_EQ)  ? BINOP_ADD :
                                   (a == TOK_MINUS_EQ) ? BINOP_SUB :
                                   (a == TOK_STAR_EQ)  ? BINOP_MUL : BINOP_DIV;
                    ast_idx_t comp = arena_alloc(&c->p->arena, AST_EXPR_BINOP);
                    c->p->arena.nodes[comp].u.binop.op = bop;
                    ast_idx_t kk[2] = { lhs, rhs };
                    set_children(&c->p->arena, comp, kk, 2);
                    rhs = comp;
                }
                ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_ASSIGN);
                ast_idx_t kk[2] = { lhs, rhs };
                set_children(&c->p->arena, n, kk, 2);
                return n;
            }
            return lhs;
        }
    }
}

/* ==========================================================================
   Query parsing: query { all { ... } any { ... } none { ... } }
   ========================================================================== */
static ast_idx_t parse_tag_clause(parse_ctx_t* c) {
    int subj = parse_subject_kw(c->p);
    if (subj < 0) subj = SUBJ_SELF;
    int exact = 0;
    if (match(c->p, TOK_KW_EXACT)) exact = 1;
    uint32_t h = 0;
    token_t t = expect(c->p, TOK_IDENT, "expected tag path");
    h = intern_tag(c->p,t.start, t.len);
    ast_idx_t n = arena_alloc(&c->p->arena, exact ? AST_CLAUSE_TAG_EXACT : AST_CLAUSE_TAG);
    c->p->arena.nodes[n].u.attr.subj     = (ast_subject_t)subj;
    c->p->arena.nodes[n].u.attr.tag_idx = h;
    return n;
}

static int looks_like_tag_clause(parser_t* p) {
    token_kind_t k = peek(p).kind;
    if (k == TOK_KW_EXACT) return 1;
    /* Subject keyword followed by IDENT (not '.') is a tag clause.
       Subject + '.' is attribute access → predicate clause. */
    if (k == TOK_KW_SELF || k == TOK_KW_TARGET || k == TOK_KW_SOURCE) {
        return peek2(p).kind != TOK_DOT;
    }
    return 0;
}

static ast_idx_t parse_query_section(parse_ctx_t* c, ast_kind_t kind) {
    expect(c->p, TOK_LBRACE, "expected '{'");
    uint32_t saved = c->kids_count;
    while (!check(c->p, TOK_RBRACE) && !check(c->p, TOK_EOF)) {
        ast_idx_t clause;
        if (looks_like_tag_clause(c->p)) {
            clause = parse_tag_clause(c);
        } else {
            /* predicate clause: expr cmp expr */
            ast_idx_t lhs = parse_expr(c);
            token_kind_t op = peek(c->p).kind;
            if (op == TOK_LT || op == TOK_LE || op == TOK_GT || op == TOK_GE ||
                op == TOK_EQ || op == TOK_NEQ) {
                lexer_next(&c->p->lex);
                ast_idx_t rhs = parse_expr(c);
                ast_idx_t n = arena_alloc(&c->p->arena, AST_CLAUSE_PRED);
                c->p->arena.nodes[n].u.binop.op = binop_of(op);
                ast_idx_t kk[2] = { lhs, rhs };
                set_children(&c->p->arena, n, kk, 2);
                clause = n;
            } else {
                /* Bare expr → treat as tag-presence shortcut: clauses already
                   absorbed plain idents as attrs; allow flagged check via has_tag(). */
                clause = lhs;
            }
        }
        kids_push(c, clause);
        match(c->p, TOK_COMMA);
        match(c->p, TOK_SEMICOLON);
    }
    expect(c->p, TOK_RBRACE, "expected '}'");
    ast_idx_t n = arena_alloc(&c->p->arena, kind);
    set_children(&c->p->arena, n, &c->kids_buf[saved], c->kids_count - saved);
    c->kids_count = saved;
    return n;
}

static ast_idx_t parse_query(parse_ctx_t* c) {
    expect(c->p, TOK_LBRACE, "expected '{'");
    uint32_t saved = c->kids_count;
    while (!check(c->p, TOK_RBRACE) && !check(c->p, TOK_EOF)) {
        if      (match(c->p, TOK_KW_ALL))  kids_push(c, parse_query_section(c, AST_QUERY_ALL));
        else if (match(c->p, TOK_KW_ANY))  kids_push(c, parse_query_section(c, AST_QUERY_ANY));
        else if (match(c->p, TOK_KW_NONE)) kids_push(c, parse_query_section(c, AST_QUERY_NONE));
        else { error(c->p, "expected all/any/none"); consume(c->p); }
    }
    expect(c->p, TOK_RBRACE, "expected '}'");
    ast_idx_t n = arena_alloc(&c->p->arena, AST_QUERY);
    set_children(&c->p->arena, n, &c->kids_buf[saved], c->kids_count - saved);
    c->kids_count = saved;
    return n;
}

/* ==========================================================================
   Block parsers for declaration sub-blocks. Each produces an AST_BLOCK_*
   node whose children are clause/expr/tag refs.
   ========================================================================== */
static ast_idx_t parse_tag_list_block(parse_ctx_t* c, ast_kind_t kind) {
    expect(c->p, TOK_LBRACE, "expected '{'");
    uint32_t saved = c->kids_count;
    while (!check(c->p, TOK_RBRACE) && !check(c->p, TOK_EOF)) {
        uint32_t h = 0;
        ast_idx_t tn = parse_tag_path(c, &h);
        kids_push(c, tn);
        match(c->p, TOK_COMMA);
        match(c->p, TOK_SEMICOLON);
    }
    expect(c->p, TOK_RBRACE, "expected '}'");
    ast_idx_t n = arena_alloc(&c->p->arena, kind);
    set_children(&c->p->arena, n, &c->kids_buf[saved], c->kids_count - saved);
    c->kids_count = saved;
    return n;
}

/* Key=expr pairs, e.g. attributes { hp = 100, mp = 50 } */
static ast_idx_t parse_kv_block(parse_ctx_t* c, ast_kind_t kind) {
    expect(c->p, TOK_LBRACE, "expected '{'");
    uint32_t saved = c->kids_count;
    while (!check(c->p, TOK_RBRACE) && !check(c->p, TOK_EOF)) {
        token_t name = expect_name(c->p, "expected key");
        expect(c->p, TOK_ASSIGN, "expected '='");
        ast_idx_t v  = parse_expr(c);
        ast_idx_t kv = arena_alloc(&c->p->arena, AST_EXPR_ASSIGN);
        c->p->arena.nodes[kv].u.tag.tag_idx = intern_tag(c->p,name.start, name.len);
        ast_idx_t kk[1] = { v };
        set_children(&c->p->arena, kv, kk, 1);
        kids_push(c, kv);
        match(c->p, TOK_COMMA);
        match(c->p, TOK_SEMICOLON);
    }
    expect(c->p, TOK_RBRACE, "expected '}'");
    ast_idx_t n = arena_alloc(&c->p->arena, kind);
    set_children(&c->p->arena, n, &c->kids_buf[saved], c->kids_count - saved);
    c->kids_count = saved;
    return n;
}

/* Single-expr block, e.g. duration { 5s }. */
static ast_idx_t parse_expr_block(parse_ctx_t* c, ast_kind_t kind) {
    expect(c->p, TOK_LBRACE, "expected '{'");
    ast_idx_t e = 0;
    if (!check(c->p, TOK_RBRACE)) e = parse_expr(c);
    expect(c->p, TOK_RBRACE, "expected '}'");
    ast_idx_t n = arena_alloc(&c->p->arena, kind);
    ast_idx_t kk[1] = { e };
    set_children(&c->p->arena, n, kk, e ? 1 : 0);
    return n;
}

/* on <tag> { script } — handler hook. */
static ast_idx_t parse_on_hook(parse_ctx_t* c) {
    expect(c->p, TOK_KW_ON, "expected 'on'");
    uint32_t h = 0;
    token_t t = expect_name(c->p, "expected event tag");
    h = intern_tag(c->p,t.start, t.len);
    ast_idx_t body = parse_block_stmts(c);
    ast_idx_t n = arena_alloc(&c->p->arena, AST_ON_HOOK);
    c->p->arena.nodes[n].u.tag.tag_idx = h;
    ast_idx_t kk[1] = { body };
    set_children(&c->p->arena, n, kk, 1);
    return n;
}

/* ==========================================================================
   Body block for entity / ability / effect — collect sub-blocks until '}'.
   Caller decides which kinds are valid for which decl type; we accept any
   and let codegen reject mismatches.
   ========================================================================== */
static ast_idx_t parse_body_block(parse_ctx_t* c) {
    expect(c->p, TOK_LBRACE, "expected '{'");
    uint32_t saved = c->kids_count;
    while (!check(c->p, TOK_RBRACE) && !check(c->p, TOK_EOF)) {
        token_kind_t k = peek(c->p).kind;
        ast_idx_t sub = 0;
        switch (k) {
            case TOK_KW_TAGS:         lexer_next(&c->p->lex); sub = parse_tag_list_block(c, AST_BLOCK_TAGS);        break;
            case TOK_KW_OWNED_TAGS:   lexer_next(&c->p->lex); sub = parse_tag_list_block(c, AST_BLOCK_OWNED_TAGS);  break;
            case TOK_KW_ATTRIBUTES:   lexer_next(&c->p->lex); sub = parse_kv_block(c, AST_BLOCK_ATTRIBUTES);        break;
            case TOK_KW_EFFECTS:      lexer_next(&c->p->lex); sub = parse_tag_list_block(c, AST_BLOCK_EFFECTS);     break;
            case TOK_KW_ABILITIES:    lexer_next(&c->p->lex); sub = parse_tag_list_block(c, AST_BLOCK_ABILITIES);   break;
            case TOK_KW_REQUIREMENTS: lexer_next(&c->p->lex); {
                ast_idx_t q = parse_query(c);
                ast_idx_t n = arena_alloc(&c->p->arena, AST_BLOCK_REQUIREMENTS);
                ast_idx_t kk[1] = { q };
                set_children(&c->p->arena, n, kk, 1);
                sub = n;
            } break;
            case TOK_KW_ONGOING: lexer_next(&c->p->lex); {
                ast_idx_t q = parse_query(c);
                ast_idx_t n = arena_alloc(&c->p->arena, AST_BLOCK_ONGOING);
                ast_idx_t kk[1] = { q };
                set_children(&c->p->arena, n, kk, 1);
                sub = n;
            } break;
            case TOK_KW_CANCEL: lexer_next(&c->p->lex); {
                ast_idx_t q = parse_query(c);
                ast_idx_t n = arena_alloc(&c->p->arena, AST_BLOCK_CANCEL);
                ast_idx_t kk[1] = { q };
                set_children(&c->p->arena, n, kk, 1);
                sub = n;
            } break;
            case TOK_KW_COSTS:    lexer_next(&c->p->lex); sub = parse_kv_block(c, AST_BLOCK_COSTS);     break;
            case TOK_KW_COOLDOWNS: lexer_next(&c->p->lex); sub = parse_kv_block(c, AST_BLOCK_COOLDOWNS); break;
            case TOK_KW_DURATION: lexer_next(&c->p->lex); sub = parse_expr_block(c, AST_BLOCK_DURATION); break;
            case TOK_KW_PERIOD:   lexer_next(&c->p->lex); sub = parse_expr_block(c, AST_BLOCK_PERIOD);   break;
            case TOK_KW_EVERY:    lexer_next(&c->p->lex); {
                ast_idx_t body = parse_block_stmts(c);
                ast_idx_t n = arena_alloc(&c->p->arena, AST_BLOCK_EVERY);
                ast_idx_t kk[1] = { body };
                set_children(&c->p->arena, n, kk, 1);
                sub = n;
            } break;
            case TOK_KW_SOURCE: lexer_next(&c->p->lex); sub = parse_tag_list_block(c, AST_BLOCK_SOURCE_CAPS); break;
            case TOK_KW_TARGET: lexer_next(&c->p->lex); sub = parse_tag_list_block(c, AST_BLOCK_TARGET_CAPS); break;
            case TOK_KW_ON:     sub = parse_on_hook(c); break;
            case TOK_IDENT: {
                /* Component-init block: `compname { field = expr; ... }`.
                   compname is a bare ident; tells prefabs which component to
                   add+initialize on the spawned entity. Field names + types
                   are resolved against the compiler's component reflection
                   table during codegen (codegen errors if unknown). */
                token_t name = lexer_next(&c->p->lex);
                uint32_t name_idx = intern_tag(c->p, name.start, name.len);
                expect(c->p, TOK_LBRACE, "expected '{' after component name");
                uint32_t fsaved = c->kids_count;
                while (!check(c->p, TOK_RBRACE) && !check(c->p, TOK_EOF)) {
                    token_t fname = expect_name(c->p, "expected field name");
                    expect(c->p, TOK_ASSIGN, "expected '='");
                    ast_idx_t fv = parse_expr(c);
                    ast_idx_t fn = arena_alloc(&c->p->arena, AST_COMPONENT_FIELD);
                    c->p->arena.nodes[fn].u.tag.tag_idx = intern_tag(c->p, fname.start, fname.len);
                    ast_idx_t fkk[1] = { fv };
                    set_children(&c->p->arena, fn, fkk, 1);
                    kids_push(c, fn);
                    match(c->p, TOK_COMMA);
                    match(c->p, TOK_SEMICOLON);
                }
                expect(c->p, TOK_RBRACE, "expected '}'");
                ast_idx_t cn = arena_alloc(&c->p->arena, AST_BLOCK_COMPONENT_INIT);
                c->p->arena.nodes[cn].u.tag.tag_idx = name_idx;
                set_children(&c->p->arena, cn, &c->kids_buf[fsaved], c->kids_count - fsaved);
                c->kids_count = fsaved;
                sub = cn;
            } break;
            default:
                error(c->p, "unexpected token in body");
                consume(c->p);
                continue;
        }
        kids_push(c, sub);
    }
    expect(c->p, TOK_RBRACE, "expected '}'");
    ast_idx_t n = arena_alloc(&c->p->arena, AST_EXPR_BLOCK);
    set_children(&c->p->arena, n, &c->kids_buf[saved], c->kids_count - saved);
    c->kids_count = saved;
    return n;
}

/* ==========================================================================
   Top-level declarations.
   ========================================================================== */
static ast_idx_t parse_prefab(parse_ctx_t* c, uint32_t name_idx) {
    ast_idx_t body = parse_body_block(c);
    ast_idx_t n = arena_alloc(&c->p->arena, AST_DECL_PREFAB);
    c->p->arena.nodes[n].u.tag.tag_idx = name_idx;
    ast_idx_t kk[1] = { body };
    set_children(&c->p->arena, n, kk, 1);
    return n;
}

static ast_idx_t parse_ability(parse_ctx_t* c, uint32_t name_idx) {
    ast_idx_t body = parse_body_block(c);
    ast_idx_t n = arena_alloc(&c->p->arena, AST_DECL_ABILITY);
    c->p->arena.nodes[n].u.tag.tag_idx = name_idx;
    ast_idx_t kk[1] = { body };
    set_children(&c->p->arena, n, kk, 1);
    return n;
}

static ast_idx_t parse_effect(parse_ctx_t* c, uint32_t name_idx) {
    ast_idx_t body = parse_body_block(c);
    ast_idx_t n = arena_alloc(&c->p->arena, AST_DECL_EFFECT);
    c->p->arena.nodes[n].u.tag.tag_idx = name_idx;
    ast_idx_t kk[1] = { body };
    set_children(&c->p->arena, n, kk, 1);
    return n;
}

/* command NAME { type1 name1; type2 name2; ... }
   Top-level decl. NAME becomes a tag with kind = TAG_KIND_COMMAND.
   Each param is an AST_COMMAND_PARAM child with `flags` = param_type_t
   and `u.tag.tag_idx` = interned param-name index. Empty body allowed. */
static ast_idx_t parse_command_decl(parse_ctx_t* c) {
    lexer_next(&c->p->lex);                                  /* consume `command` */
    token_t name = expect_name(c->p, "expected command name");
    uint32_t name_idx = intern_tag(c->p, name.start, name.len);
    expect(c->p, TOK_LBRACE, "expected '{'");
    uint32_t saved = c->kids_count;
    while (!check(c->p, TOK_RBRACE) && !check(c->p, TOK_EOF)) {
        token_kind_t tk = peek(c->p).kind;
        uint8_t pt;
        if      (tk == TOK_KW_TYPE_ENTITY) pt = (uint8_t)PARAM_TYPE_ENTITY;
        else if (tk == TOK_KW_TYPE_POINT)  pt = (uint8_t)PARAM_TYPE_POINT;
        else {
            error(c->p, "expected param type ('entity' or 'point')");
            consume(c->p);
            continue;
        }
        lexer_next(&c->p->lex);
        /* Field name: any ident or reserved-keyword text — `target`,
           `source`, `position`, etc. all accepted. */
        token_t pn = expect_name(c->p, "expected param name");
        uint32_t pidx = intern_tag(c->p, pn.start, pn.len);
        match(c->p, TOK_SEMICOLON);
        match(c->p, TOK_COMMA);
        ast_idx_t pnode = arena_alloc(&c->p->arena, AST_COMMAND_PARAM);
        c->p->arena.nodes[pnode].flags         = pt;
        c->p->arena.nodes[pnode].u.tag.tag_idx = pidx;
        kids_push(c, pnode);
    }
    expect(c->p, TOK_RBRACE, "expected '}'");
    ast_idx_t n = arena_alloc(&c->p->arena, AST_DECL_COMMAND);
    c->p->arena.nodes[n].u.tag.tag_idx = name_idx;
    set_children(&c->p->arena, n, &c->kids_buf[saved], c->kids_count - saved);
    c->kids_count = saved;
    return n;
}

/* input { (button|stick) name , ... } — anonymous top-level decl.
   One global per-game; emits AST_INPUT_BUTTON / AST_INPUT_STICK children,
   each carrying the slot's interned tag idx. */
static ast_idx_t parse_input(parse_ctx_t* c) {
    lexer_next(&c->p->lex);                      /* consume `input` */
    expect(c->p, TOK_LBRACE, "expected '{'");
    uint32_t saved = c->kids_count;
    while (!check(c->p, TOK_RBRACE) && !check(c->p, TOK_EOF)) {
        token_kind_t k = peek(c->p).kind;
        ast_kind_t   ak;
        if (k == TOK_KW_BUTTON)      ak = AST_INPUT_BUTTON;
        else if (k == TOK_KW_STICK)  ak = AST_INPUT_STICK;
        else {
            error(c->p, "expected 'button' or 'stick'");
            consume(c->p);
            continue;
        }
        lexer_next(&c->p->lex);
        token_t name = expect_name(c->p, "expected slot name");
        uint32_t h = intern_tag(c->p, name.start, name.len);
        ast_idx_t entry = arena_alloc(&c->p->arena, ak);
        c->p->arena.nodes[entry].u.tag.tag_idx = h;
        kids_push(c, entry);
        match(c->p, TOK_COMMA);
        match(c->p, TOK_SEMICOLON);
    }
    expect(c->p, TOK_RBRACE, "expected '}'");
    ast_idx_t n = arena_alloc(&c->p->arena, AST_DECL_INPUT);
    set_children(&c->p->arena, n, &c->kids_buf[saved], c->kids_count - saved);
    c->kids_count = saved;
    return n;
}

static ast_idx_t parse_decl(parse_ctx_t* c) {
    token_kind_t k = peek(c->p).kind;
    /* Top-level decls with body but no decl-name token (anonymous singletons). */
    if (k == TOK_KW_INPUT)   return parse_input(c);
    /* `command NAME { ... }` — has a name but is parsed separately so we can
       enforce typed param parsing in the body. */
    if (k == TOK_KW_COMMAND) return parse_command_decl(c);
    if (k != TOK_KW_PREFAB && k != TOK_KW_ABILITY && k != TOK_KW_EFFECT) {
        error(c->p, "expected 'prefab', 'ability', 'effect', 'command', or 'input'");
        consume(c->p);
        return 0;
    }
    lexer_next(&c->p->lex);
    token_t name = expect_name(c->p, "expected declaration name");
    uint32_t h = intern_tag(c->p,name.start, name.len);
    switch (k) {
        case TOK_KW_PREFAB:  return parse_prefab (c, h);
        case TOK_KW_ABILITY: return parse_ability(c, h);
        case TOK_KW_EFFECT:  return parse_effect (c, h);
        default: return 0;
    }
}

/* ==========================================================================
   Public API.
   ========================================================================== */
void parser_init(parser_t* p, const char* src, uint32_t len, const char* filename) {
    memset(p, 0, sizeof(*p));
    lexer_init(&p->lex, src, len, SCAN_FULL);
    p->filename = filename;
}

void parser_destroy(parser_t* p) {
    ecs_free(p->arena.nodes);       p->arena.nodes = NULL;
    ecs_free(p->arena.child_runs);  p->arena.child_runs = NULL;
    ecs_free(p->tag_strs);          p->tag_strs    = NULL;
    ecs_free(p->tag_strs_next);     p->tag_strs_next = NULL;
    p->arena.count = p->arena.cap = 0;
    p->arena.child_runs_count = p->arena.child_runs_cap = 0;
    p->tag_strs_count = p->tag_strs_cap = 0;
}

int parser_run(parser_t* p) {
    parse_ctx_t c = { .p = p };
    /* arena_alloc lazy-reserves slot 0 internally on first call; no priming needed. */
    uint32_t saved = c.kids_count;
    while (!check(p, TOK_EOF)) {
        ast_idx_t d = parse_decl(&c);
        if (d) kids_push(&c, d);
    }
    ast_idx_t root = arena_alloc(&p->arena, AST_EXPR_BLOCK);
    set_children(&p->arena, root, &c.kids_buf[saved], c.kids_count - saved);
    /* Store root index at slot 0 children[0] for retrieval. */
    p->arena.nodes[0].children[0] = root;
    ecs_free(c.kids_buf);
    return !p->had_error;
}

ast_idx_t parser_root(const parser_t* p) {
    if (p->arena.count < 1) return 0;
    return p->arena.nodes[0].children[0];
}
