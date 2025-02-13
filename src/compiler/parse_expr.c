// Copyright (c) 2020 Christoffer Lerno. All rights reserved.
// Use of this source code is governed by a LGPLv3.0
// a copy of which can be found in the LICENSE file.

#include "compiler_internal.h"
#include "parser_internal.h"


typedef Expr *(*ParseFn)(Context *context, Expr *);

typedef struct
{
	ParseFn prefix;
	ParseFn infix;
	Precedence precedence;
} ParseRule;

extern ParseRule rules[TOKEN_EOF + 1];


inline Expr *parse_precedence_with_left_side(Context *context, Expr *left_side, Precedence precedence)
{
	while (precedence <= rules[context->tok.type].precedence)
	{
		if (!expr_ok(left_side)) return left_side;
		ParseFn infix_rule = rules[context->tok.type].infix;
		if (!infix_rule)
		{
			SEMA_TOKEN_ERROR(context->tok, "An expression was expected.");
			return poisoned_expr;
		}
		left_side = infix_rule(context, left_side);
	}
	return left_side;
}


static Expr *parse_precedence(Context *context, Precedence precedence)
{
	// Get the rule for the previous token.
	ParseFn prefix_rule = rules[context->tok.type].prefix;
	if (prefix_rule == NULL)
	{
		SEMA_TOKEN_ERROR(context->tok, "An expression was expected.");
		return poisoned_expr;
	}

	Expr *left_side = prefix_rule(context, NULL);
	if (!expr_ok(left_side)) return left_side;
	return parse_precedence_with_left_side(context, left_side, precedence);
}

static inline Expr *parse_expr_or_initializer_list(Context *context)
{
	if (TOKEN_IS(TOKEN_LBRACE))
	{
		return parse_initializer_list(context);
	}
	return parse_expr(context);
}

inline Expr* parse_expr(Context *context)
{
	return parse_precedence(context, PREC_ASSIGNMENT);
}

inline Expr* parse_constant_expr(Context *context)
{
	return parse_precedence(context, PREC_TERNARY);
}

/**
 * param_path : ('[' expression ']' | '.' IDENT)*
 *
 * @param context
 * @param path reference to the path to return
 * @return true if parsing succeeds, false otherwise.
 */
static bool parse_param_path(Context *context, DesignatorElement ***path)
{
	*path = NULL;
	while (true)
	{
		if (TOKEN_IS(TOKEN_LBRACKET))
		{
			// Parse the inside of [ ]
			DesignatorElement *element = CALLOCS(DesignatorElement);
			element->kind = DESIGNATOR_ARRAY;
			advance_and_verify(context, TOKEN_LBRACKET);
			element->index_expr = TRY_EXPR_OR(parse_expr(context), false);
			// Possible range
			if (try_consume(context, TOKEN_DOTDOT))
			{
				element->index_end_expr = TRY_EXPR_OR(parse_expr(context), false);
				element->kind = DESIGNATOR_RANGE;
			}
			CONSUME_OR(TOKEN_RBRACKET, false);
			// Include ] in the expr
			vec_add(*path, element);
			continue;
		}
		if (TOKEN_IS(TOKEN_DOT))
		{
			advance(context);
			DesignatorElement *element = CALLOCS(DesignatorElement);
			element->kind = DESIGNATOR_FIELD;
			element->field = TOKSTR(context->tok.id);
			EXPECT_OR(TOKEN_IDENT, false);
			advance(context);
			vec_add(*path, element);
			continue;
		}
		return true;
	}
}
/**
 * param_list ::= ('...' parameter | parameter (',' parameter)*)?
 *
 * parameter ::= (param_path '=')? expr
 */
bool parse_arg_list(Context *context, Expr ***result, TokenType param_end, bool *unsplat)
{
	*result = NULL;
	if (unsplat) *unsplat = false;
	while (1)
	{
		Expr *expr = NULL;
		DesignatorElement **path;
		Token current = context->tok;
		if (!parse_param_path(context, &path)) return false;
		if (path != NULL)
		{
			// Create the parameter expr
			expr = EXPR_NEW_TOKEN(EXPR_DESIGNATOR, current);
			expr->designator_expr.path = path;
			RANGE_EXTEND_PREV(expr);

			// Expect the '=' after.
			CONSUME_OR(TOKEN_EQ, false);

			// Now parse the rest
			expr->designator_expr.value = TRY_EXPR_OR(parse_expr_or_initializer_list(context), false);
		}
		else
		{
			if (unsplat)
			{
				*unsplat = try_consume(context, TOKEN_ELLIPSIS);
			}
			expr = TRY_EXPR_OR(parse_expr_or_initializer_list(context), false);
		}
		vec_add(*result, expr);
		if (!try_consume(context, TOKEN_COMMA))
		{
			return true;
		}
		if (TOKEN_IS(param_end)) return true;
		if (unsplat && *unsplat)
		{
			SEMA_TOKEN_ERROR(context->tok, "'...' is only allowed on the last argument in a call.");
			return false;
		}
	}
}

/**
 * macro_expansion ::= '@' non_at_expression
 */
static Expr *parse_macro_expansion(Context *context, Expr *left)
{
	assert(!left && "Unexpected left hand side");
	Expr *macro_expression = EXPR_NEW_TOKEN(EXPR_MACRO_EXPANSION, context->tok);
	advance_and_verify(context, TOKEN_AT);
	Expr *inner = TRY_EXPR_OR(parse_precedence(context, PREC_MACRO), poisoned_expr);
	macro_expression->macro_expansion_expr.inner = inner;
	assert(inner);
	RANGE_EXTEND_PREV(macro_expression);
	return macro_expression;
}


/**
 * expression_list
 *	: expression
 *	| expression_list ',' expression
 *	;
 * @return Ast *
 */
Expr *parse_expression_list(Context *context)
{
	Expr *expr_list = EXPR_NEW_TOKEN(EXPR_EXPRESSION_LIST, context->tok);
	while (1)
	{
		Expr *expr = NULL;
		expr = TRY_EXPR_OR(parse_expr_or_initializer_list(context), poisoned_expr);
		vec_add(expr_list->expression_list, expr);
		if (!try_consume(context, TOKEN_COMMA)) break;
	}
	return expr_list;
}

/**
 * @param left must be null.
 * @return Expr*
 */
static Expr *parse_type_identifier(Context *context, Expr *left)
{
	assert(!left && "Unexpected left hand side");
	return parse_type_expression_with_path(context, NULL);
}

static Expr *parse_typeof_expr(Context *context, Expr *left)
{
	assert(!left && "Unexpected left hand side");
	Expr *expr = EXPR_NEW_TOKEN(EXPR_TYPEINFO, context->tok);
	TypeInfo *type = TRY_TYPE_OR(parse_type(context), poisoned_expr);
	expr->span = type->span;
	expr->type_expr = type;
	return expr;
}


static Expr *parse_unary_expr(Context *context, Expr *left)
{
	assert(!left && "Did not expect a left hand side!");

	TokenType operator_type = context->tok.type;

	Expr *unary = EXPR_NEW_TOKEN(EXPR_UNARY, context->tok);
	unary->unary_expr.operator = unaryop_from_token(operator_type);
	advance(context);
	Expr *right_side = parse_precedence(context, PREC_UNARY);

	CHECK_EXPR(right_side);

	unary->unary_expr.expr = right_side;
	unary->span.end_loc = right_side->span.end_loc;
	return unary;
}

static Expr *parse_post_unary(Context *context, Expr *left)
{
	assert(expr_ok(left));
	Expr *unary = EXPR_NEW_TOKEN(EXPR_POST_UNARY, context->tok);
	unary->post_expr.expr = left;
	unary->post_expr.operator = post_unaryop_from_token(context->tok.type);
	unary->span.loc = left->span.loc;
	advance(context);
	return unary;
}




static Expr *parse_ternary_expr(Context *context, Expr *left_side)
{
	assert(expr_ok(left_side));

	Expr *expr_ternary = EXPR_NEW_EXPR(EXPR_TERNARY, left_side);
	expr_ternary->ternary_expr.cond = left_side;


	// Check for elvis
	if (try_consume(context, TOKEN_ELVIS))
	{
		expr_ternary->ternary_expr.then_expr = NULL;
	}
	else
	{
		advance_and_verify(context, TOKEN_QUESTION);
		Expr *true_expr = TRY_EXPR_OR(parse_precedence(context, PREC_TERNARY + 1), poisoned_expr);
		expr_ternary->ternary_expr.then_expr = true_expr;
		CONSUME_OR(TOKEN_COLON, poisoned_expr);
	}

	Expr *false_expr = TRY_EXPR_OR(parse_precedence(context, PREC_TERNARY + 1), poisoned_expr);
	expr_ternary->ternary_expr.else_expr = false_expr;
	RANGE_EXTEND_PREV(expr_ternary);
	return expr_ternary;
}

/**
 * grouping_expr
 * 	: '(' expression ')' ('(' expression ')')?
 * 	;
 */
static Expr *parse_grouping_expr(Context *context, Expr *left)
{
	assert(!left && "Unexpected left hand side");
	Expr *expr = EXPR_NEW_TOKEN(EXPR_GROUP, context->tok);
	advance_and_verify(context, TOKEN_LPAREN);
	expr->group_expr = TRY_EXPR_OR(parse_expr(context), poisoned_expr);
	CONSUME_OR(TOKEN_RPAREN, poisoned_expr);
	if (expr->group_expr->expr_kind == EXPR_TYPEINFO && try_consume(context, TOKEN_LPAREN))
	{
		TypeInfo *info = expr->group_expr->type_expr;
		if (TOKEN_IS(TOKEN_LBRACE) && info->resolve_status != RESOLVE_DONE)
		{
			SEMA_TOKEN_ERROR(context->tok, "Unexpected start of a block '{' here. If you intended a compound literal, remove the () around the type.");
			return poisoned_expr;
		}
		Expr *cast_expr = TRY_EXPR_OR(parse_expr(context), poisoned_expr);
		CONSUME_OR(TOKEN_RPAREN, poisoned_expr);
		expr->expr_kind = EXPR_CAST;
		expr->cast_expr.type_info = info;
		expr->cast_expr.expr = cast_expr;
	}
	RANGE_EXTEND_PREV(expr);
	return expr;
}

/**
 * initializer
 *  : initializer_list
 *  | expr
 *  | void
 *  ;
 *
 * @param context
 * @return the parsed expression
 */
Expr *parse_initializer(Context *context)
{
	if (TOKEN_IS(TOKEN_VOID))
	{
		Expr *expr = EXPR_NEW_TOKEN(EXPR_UNDEF, context->tok);
		expr->type = type_void;
		expr->resolve_status = RESOLVE_DONE;
		advance(context);
		return expr;
	}
	if (TOKEN_IS(TOKEN_LBRACE))
	{
		return parse_initializer_list(context);
	}
	return parse_expr(context);
}

/**
 * initializer_list
 * 	: '{' initializer_values '}'
 *	| '{' initializer_values ',' '}'
 *	;
 *
 * initializer_values
 *	: initializer
 *	| initializer_values ',' initializer
 *	;
 *
 * @param elements
 * @return
 */
Expr *parse_initializer_list(Context *context)
{
	Expr *initializer_list = EXPR_NEW_TOKEN(EXPR_INITIALIZER_LIST, context->tok);
	initializer_list->initializer_expr.init_type = INITIALIZER_UNKNOWN;
	CONSUME_OR(TOKEN_LBRACE, poisoned_expr);
	if (!try_consume(context, TOKEN_RBRACE))
	{
		if (!parse_arg_list(context, &initializer_list->initializer_expr.initializer_expr, TOKEN_RBRACE, NULL)) return poisoned_expr;
		CONSUME_OR(TOKEN_RBRACE, poisoned_expr);
	}
	RANGE_EXTEND_PREV(initializer_list);
	return initializer_list;
}

static Expr *parse_failable(Context *context, Expr *left_side)
{
	Expr *failable = expr_new(EXPR_FAILABLE, left_side->span);
	advance_and_verify(context, TOKEN_BANG);
	failable->failable_expr = left_side;
	RANGE_EXTEND_PREV(failable);
	return failable;
}



static Expr *parse_binary(Context *context, Expr *left_side)
{
	assert(left_side && expr_ok(left_side));

	// Remember the operator.
	TokenType operator_type = context->tok.type;

	advance(context);

	Expr *right_side;
	if (TOKEN_IS(TOKEN_LBRACE) && operator_type == TOKEN_EQ)
	{
		right_side = TRY_EXPR_OR(parse_initializer_list(context), poisoned_expr);
	}
	else
	{
		// Assignment operators have precedence right -> left.
		if (rules[operator_type].precedence == PREC_ASSIGNMENT)
		{
			right_side = TRY_EXPR_OR(parse_precedence(context, PREC_ASSIGNMENT), poisoned_expr);
		}
		else
		{
			right_side = TRY_EXPR_OR(parse_precedence(context, rules[operator_type].precedence + 1), poisoned_expr);
		}
	}

	Expr *expr = EXPR_NEW_EXPR(EXPR_BINARY, left_side);
	expr->binary_expr.operator = binaryop_from_token(operator_type);
	expr->binary_expr.left = left_side;
	expr->binary_expr.right = right_side;
	
	expr->span.end_loc = right_side->span.end_loc;
	return expr;
}

static Expr *parse_call_expr(Context *context, Expr *left)
{
	assert(left && expr_ok(left));

	Expr **params = NULL;
	advance_and_verify(context, TOKEN_LPAREN);
	bool unsplat = false;
	Decl **body_args = NULL;
	if (!TOKEN_IS(TOKEN_RPAREN))
	{
		if (!parse_arg_list(context, &params, TOKEN_RPAREN, &unsplat)) return poisoned_expr;
	}
	if (try_consume(context, TOKEN_EOS) && parse_next_is_type(context))
	{
		if (!parse_parameters(context, VISIBLE_LOCAL, &body_args)) return poisoned_expr;
	}
	if (!TOKEN_IS(TOKEN_RPAREN))
	{
		SEMA_TOKID_ERROR(context->prev_tok, "Expected the ending ')' here.");
		return poisoned_expr;
	}
	advance(context);

	Expr *call = EXPR_NEW_EXPR(EXPR_CALL, left);
	call->call_expr.function = left;
	call->call_expr.arguments = params;
	call->call_expr.unsplat_last = unsplat;
	call->call_expr.body_arguments = body_args;
	RANGE_EXTEND_PREV(call);
	if (body_args && !TOKEN_IS(TOKEN_LBRACE))
	{
		SEMA_TOKEN_ERROR(context->tok, "Expected a macro body here.");
		return poisoned_expr;
	}
	if (TOKEN_IS(TOKEN_LBRACE))
	{
		call->call_expr.body = TRY_AST_OR(parse_compound_stmt(context), poisoned_expr);
	}
	if (!parse_attributes(context, &call->call_expr.attributes)) return false;
	return call;
}



static Expr *parse_subscript_expr(Context *context, Expr *left)
{
	assert(left && expr_ok(left));
	advance_and_verify(context, TOKEN_LBRACKET);

	Expr *subs_expr = EXPR_NEW_EXPR(EXPR_SUBSCRIPT, left);
	Expr *index = NULL;
	bool is_range = false;
	bool from_back = false;
	bool end_from_back = false;
	Expr *end = NULL;

	// Not range with missing entry
	if (!TOKEN_IS(TOKEN_DOTDOT))
	{
		// Might be ^ prefix
		from_back = try_consume(context, TOKEN_BIT_XOR);
		index = TRY_EXPR_OR(parse_expr(context), poisoned_expr);
	}
	else
	{
		index = EXPR_NEW_TOKEN(EXPR_CONST, context->tok);
		expr_set_type(index, type_uint);
		index->constant = true;
		index->resolve_status = RESOLVE_DONE;
		expr_const_set_int(&index->const_expr, 0, type_uint->type_kind);
	}
	if (try_consume(context, TOKEN_DOTDOT))
	{
		is_range = true;
		if (!TOKEN_IS(TOKEN_RBRACKET))
		{
			end_from_back = try_consume(context, TOKEN_BIT_XOR);
			end = TRY_EXPR_OR(parse_expr(context), poisoned_expr);
		}
	}
	CONSUME_OR(TOKEN_RBRACKET, poisoned_expr);
	RANGE_EXTEND_PREV(subs_expr);

	if (is_range)
	{
		subs_expr->expr_kind = EXPR_SLICE;
		subs_expr->slice_expr.expr = left;
		subs_expr->slice_expr.start = index;
		subs_expr->slice_expr.start_from_back = from_back;
		subs_expr->slice_expr.end = end;
		subs_expr->slice_expr.end_from_back = end_from_back;
	}
	else
	{
		subs_expr->subscript_expr.expr = left;
		subs_expr->subscript_expr.index = index;
		subs_expr->subscript_expr.from_back = from_back;
	}
	return subs_expr;
}


static Expr *parse_access_expr(Context *context, Expr *left)
{
	assert(left && expr_ok(left));
	advance_and_verify(context, TOKEN_DOT);
	Expr *access_expr = EXPR_NEW_EXPR(EXPR_ACCESS, left);
	access_expr->access_expr.parent = left;
	access_expr->access_expr.child = TRY_EXPR_OR(parse_precedence(context, PREC_CALL + 1), poisoned_expr);
	RANGE_EXTEND_PREV(access_expr);
	return access_expr;
}



static Expr *parse_ct_ident(Context *context, Expr *left)
{
	assert(!left && "Unexpected left hand side");
	if (try_consume(context, TOKEN_CT_CONST_IDENT))
	{
		SEMA_TOKID_ERROR(context->prev_tok, "Compile time identifiers may not be constants.");
		return poisoned_expr;
	}
	Expr *expr = EXPR_NEW_TOKEN(EXPR_CT_IDENT, context->tok);
	expr->ct_ident_expr.identifier = context->tok.id;
	advance(context);
	return expr;
}

static Expr *parse_hash_ident(Context *context, Expr *left)
{
	assert(!left && "Unexpected left hand side");
	Expr *expr = EXPR_NEW_TOKEN(EXPR_HASH_IDENT, context->tok);
	expr->ct_ident_expr.identifier = context->tok.id;
	advance(context);
	return expr;
}

static Expr *parse_ct_call(Context *context, Expr *left)
{
	assert(!left && "Unexpected left hand side");
	Expr *expr = EXPR_NEW_TOKEN(EXPR_CT_CALL, context->tok);
	expr->ct_call_expr.token_type = context->tok.type;
	advance(context);
	CONSUME_OR(TOKEN_LPAREN, poisoned_expr);
	Expr *element = TRY_EXPR_OR(parse_expr(context), poisoned_expr);
	vec_add(expr->ct_call_expr.arguments, element);
	if (try_consume(context, TOKEN_COMMA))
	{
		if (context->tok.type == TOKEN_IDENT || context->tok.type == TOKEN_LBRACKET)
		{
			Expr *idents = EXPR_NEW_TOKEN(EXPR_FLATPATH, context->tok);
			bool first = true;
			while (1)
			{
				ExprFlatElement flat_element;
				if (try_consume(context, TOKEN_LBRACKET))
				{
					Expr *int_expr = TRY_EXPR_OR(parse_expr(context), poisoned_expr);
					if (int_expr->expr_kind != EXPR_CONST || !type_kind_is_any_integer(int_expr->const_expr.kind))
					{
						SEMA_TOKEN_ERROR(context->tok, "Expected an integer index.");
						return poisoned_expr;
					}
					BigInt *value = &int_expr->const_expr.i;
					BigInt limit;
					bigint_init_unsigned(&limit, MAX_ARRAYINDEX);
					if (bigint_cmp(value, &limit) == CMP_GT)
					{
						SEMA_ERROR(int_expr, "Array index out of range.");
						return poisoned_expr;
					}
					if (bigint_cmp_zero(value) == CMP_LT)
					{
						SEMA_ERROR(int_expr, "Array index must be zero or greater.");
						return poisoned_expr;
					}
					TRY_CONSUME_OR(TOKEN_RBRACKET, "Expected a ']' after the number.", poisoned_expr);
					flat_element.array = true;
					flat_element.index = (ArrayIndex)bigint_as_unsigned(value);
				}
				else if (try_consume(context, TOKEN_DOT) || first)
				{
					TRY_CONSUME_OR(TOKEN_IDENT, "Expected an identifier here.", poisoned_expr);
					flat_element.array = false;
					flat_element.ident = TOKSTR(context->prev_tok);
				}
				else
				{
					SEMA_TOKEN_ERROR(context->tok, "Expected '.' or '[' here.");
					return poisoned_expr;
				}
				first = false;
				vec_add(idents->flatpath_expr, flat_element);
				if (TOKEN_IS(TOKEN_RPAREN)) break;
			}
			vec_add(expr->ct_call_expr.arguments, idents);
			RANGE_EXTEND_PREV(expr);
		}
		else
		{
			Expr *idents = TRY_EXPR_OR(parse_expr(context), poisoned_expr);
			vec_add(expr->ct_call_expr.arguments, idents);
		}
	}
	CONSUME_OR(TOKEN_RPAREN, poisoned_expr);
	RANGE_EXTEND_PREV(expr);
	return expr;
}

static Expr *parse_identifier(Context *context, Expr *left)
{
	assert(!left && "Unexpected left hand side");
	Expr *expr = EXPR_NEW_TOKEN(context->tok.type == TOKEN_CONST_IDENT ? EXPR_CONST_IDENTIFIER : EXPR_IDENTIFIER , context->tok);
	expr->identifier_expr.identifier = context->tok.id;
	advance(context);
	return expr;
}


static Expr *parse_identifier_starting_expression(Context *context, Expr *left)
{
	assert(!left && "Unexpected left hand side");
	bool had_error;
	Path *path = parse_path_prefix(context, &had_error);
	if (had_error) return poisoned_expr;
	switch (context->tok.type)
	{
		case TOKEN_IDENT:
		case TOKEN_CONST_IDENT:
		{
			Expr *expr = parse_identifier(context, NULL);
			expr->identifier_expr.path = path;
			return expr;
		}
		case TOKEN_TYPE_IDENT:
			return parse_type_expression_with_path(context, path);
		default:
			SEMA_TOKEN_ERROR(context->tok, "Expected a type, function or constant.");
			return poisoned_expr;
	}
}

static Expr *parse_try_old_expr(Context *context, Expr *left)
{
	assert(!left && "Unexpected left hand side");
	Expr *try_expr = EXPR_NEW_TOKEN(TOKEN_IS(TOKEN_TRY_OLD) ? EXPR_TRY_OLD : EXPR_CATCH_OLD, context->tok);
	advance(context);
	CONSUME_OR(TOKEN_LPAREN, poisoned_expr);
	try_expr->trycatch_expr = TRY_EXPR_OR(parse_expr(context), poisoned_expr);
	CONSUME_OR(TOKEN_RPAREN, poisoned_expr);
	return try_expr;
}

static Expr *parse_try_expr(Context *context, Expr *left)
{
	assert(!left && "Unexpected left hand side");
	bool try = TOKEN_IS(TOKEN_TRY);
	Expr *try_expr = EXPR_NEW_TOKEN(EXPR_TRY, context->tok);
	advance(context);
	Expr *expr = TRY_EXPR_OR(parse_precedence(context, PREC_UNARY), poisoned_expr);
	if (try_consume(context, TOKEN_EQ))
	{
		Expr *init = TRY_EXPR_OR(parse_precedence(context, PREC_ASSIGNMENT), poisoned_expr);
		try_expr->expr_kind = EXPR_TRY_ASSIGN;
		try_expr->try_assign_expr.expr = expr;
		try_expr->try_assign_expr.is_try = try;
		try_expr->try_assign_expr.init = init;
	}
	else
	{
		try_expr->try_expr.expr = expr;
		try_expr->try_expr.is_try = try;
	}
	RANGE_EXTEND_PREV(try_expr);
	return try_expr;
}

static Expr *parse_bangbang_expr(Context *context, Expr *left)
{
	Expr *guard_expr = EXPR_NEW_EXPR(EXPR_GUARD, left);
	advance_and_verify(context, TOKEN_BANGBANG);
	guard_expr->guard_expr.inner = left;
	RANGE_EXTEND_PREV(guard_expr);
	return guard_expr;
}

static Expr *parse_else_expr(Context *context, Expr *left)
{
	Expr *else_expr = EXPR_NEW_TOKEN(EXPR_ELSE, context->tok);
	advance_and_verify(context, TOKEN_ELSE);
	else_expr->else_expr.expr = left;
	switch (context->tok.type)
	{
		case TOKEN_RETURN:
		case TOKEN_BREAK:
		case TOKEN_CONTINUE:
		case TOKEN_NEXTCASE:
		{
			Ast *ast = TRY_AST_OR(parse_jump_stmt_no_eos(context), poisoned_expr);
			else_expr->else_expr.is_jump = true;
			else_expr->else_expr.else_stmt = ast;
			if (!TOKEN_IS(TOKEN_EOS))
			{
				SEMA_ERROR(ast, "An else jump statement must end with a ';'");
				return poisoned_expr;
			}
			break;
		}
		default:
			else_expr->else_expr.else_expr = TRY_EXPR_OR(parse_precedence(context, PREC_ASSIGNMENT), poisoned_expr);
			break;
	}
	return else_expr;
}

static Expr *parse_placeholder(Context *context, Expr *left)
{
	assert(!left && "Had left hand side");
	advance_and_verify(context, TOKEN_PLACEHOLDER);
	Expr *expr = TRY_EXPR_OR(parse_expr(context), poisoned_expr);
	CONSUME_OR(TOKEN_RBRACE, poisoned_expr);

	if (expr->expr_kind != EXPR_IDENTIFIER && TOKTYPE(expr->identifier_expr.identifier) != TOKEN_CONST_IDENT)
	{
		SEMA_ERROR(expr, "Expected an uppercase identifier that corresponds to a compile time argument.");
		return poisoned_expr;
	}
	ExprPlaceholder placeholder = { .identifier = expr->identifier_expr.identifier, .path = expr->identifier_expr.path };
	expr->placeholder_expr = placeholder;
	expr->expr_kind = EXPR_PLACEHOLDER;
	expr->resolve_status = RESOLVE_NOT_DONE;
	return expr;
}

static Expr *parse_integer(Context *context, Expr *left)
{
	assert(!left && "Had left hand side");
	Expr *expr_int = EXPR_NEW_TOKEN(EXPR_CONST, context->tok);
	const char *string = TOKSTR(context->tok);
	const char *end = string + TOKLEN(context->tok);

	BigInt *i = &expr_int->const_expr.i;
	bigint_init_unsigned(i, 0);
	BigInt diff;
	bigint_init_unsigned(&diff, 0);
	BigInt ten;
	bigint_init_unsigned(&ten, 10);
	BigInt res;
	switch (TOKLEN(context->tok) > 2 ? string[1] : '0')
	{
		case 'x':
			string += 2;
			while (string < end)
			{
				char c = *(string++);
				if (c == '_') continue;
				bigint_shl_int(&res, i, 4);
				if (c < 'A')
				{
					bigint_init_unsigned(&diff, c - '0');
				}
				else if (c < 'a')
				{
					bigint_init_unsigned(&diff, c - 'A' + 10);
				}
				else
				{
					bigint_init_unsigned(&diff, c - 'a' + 10);
				}
				bigint_add(i, &res, &diff);
			}
			break;
		case 'o':
			string += 2;
			while (string < end)
			{
				char c = *(string++);
				if (c == '_') continue;
				bigint_shl_int(&res, i, 4);
				bigint_init_unsigned(&diff, c - '0');
				bigint_add(i, &res, &diff);
			}
			break;
		case 'b':
			string += 2;
			while (string < end)
			{
				char c = *(string++);
				if (c == '_') continue;
				bigint_shl_int(&res, i, 1);
				bigint_init_unsigned(&diff, c - '0');
				bigint_add(i, &res, &diff);
			}
			break;
		default:
			while (string < end)
			{
				char c = *(string++);
				if (c == '_') continue;
				bigint_mul(&res, i, &ten);
				bigint_init_unsigned(&diff, c - '0');
				bigint_add(i, &res, &diff);
			}
			break;
	}
	expr_int->const_expr.kind = TYPE_IXX;
	expr_set_type(expr_int, type_compint);
	advance(context);
	return expr_int;
}

static Expr *parse_char_lit(Context *context, Expr *left)
{
	assert(!left && "Had left hand side");
	Expr *expr_int = EXPR_NEW_TOKEN(EXPR_CONST, context->tok);
	TokenData *data = tokendata_from_id(context->tok.id);
	switch (data->width)
	{
		case 1:
			expr_const_set_int(&expr_int->const_expr, data->char_lit.u8, TYPE_IXX);
			expr_set_type(expr_int, type_compint);
			break;
		case 2:
			expr_const_set_int(&expr_int->const_expr, data->char_lit.u16, TYPE_IXX);
			expr_set_type(expr_int, type_compint);
			break;
		case 4:
			expr_const_set_int(&expr_int->const_expr, data->char_lit.u32, TYPE_IXX);
			expr_set_type(expr_int, type_compint);
			break;
		case 8:
			expr_const_set_int(&expr_int->const_expr, data->char_lit.u64, TYPE_U64);
			expr_set_type(expr_int, type_compint);
			break;
		default:
			UNREACHABLE
	}

	advance(context);
	return expr_int;
}


static Expr *parse_double(Context *context, Expr *left)
{
	assert(!left && "Had left hand side");
	Expr *number = EXPR_NEW_TOKEN(EXPR_CONST, context->tok);
	number->const_expr.f = TOKREAL(context->tok.id);
	expr_set_type(number, type_compfloat);
	number->const_expr.kind = TYPE_FXX;
	advance(context);
	return number;
}

static int append_esc_string_token(char *restrict dest, const char *restrict src, size_t *pos)
{
	int scanned;
	uint64_t unicode_char;
	signed char scanned_char = is_valid_escape(src[0]);
	if (scanned_char < 0) return -1;
	switch (scanned_char)
	{
		case 'x':
		{
			int h = char_to_nibble(src[1]);
			int l = char_to_nibble(src[2]);
			if (h < 0 || l < 0) return -1;
			unicode_char = ((unsigned) h << 4U) + l;
			scanned = 3;
			break;
		}
		case 'u':
		{
			int x1 = char_to_nibble(src[1]);
			int x2 = char_to_nibble(src[2]);
			int x3 = char_to_nibble(src[3]);
			int x4 = char_to_nibble(src[4]);
			if (x1 < 0 || x2 < 0 || x3 < 0 || x4 < 0) return -1;
			unicode_char = ((unsigned) x1 << 12U) + ((unsigned) x2 << 8U) + ((unsigned) x3 << 4U) + x4;
			scanned = 5;
			break;
		}
		case 'U':
		{
			int x1 = char_to_nibble(src[1]);
			int x2 = char_to_nibble(src[2]);
			int x3 = char_to_nibble(src[3]);
			int x4 = char_to_nibble(src[4]);
			int x5 = char_to_nibble(src[5]);
			int x6 = char_to_nibble(src[6]);
			int x7 = char_to_nibble(src[7]);
			int x8 = char_to_nibble(src[8]);
			if (x1 < 0 || x2 < 0 || x3 < 0 || x4 < 0 || x5 < 0 || x6 < 0 || x7 < 0 || x8 < 0) return -1;
			unicode_char = ((unsigned) x1 << 28U) + ((unsigned) x2 << 24U) + ((unsigned) x3 << 20U) + ((unsigned) x4 << 16U) +
			               ((unsigned) x5 << 12U) + ((unsigned) x6 << 8U) + ((unsigned) x7 << 4U) + x8;
			scanned = 9;
			break;
		}
		default:
			dest[(*pos)++] = scanned_char;
			return 1;
	}
	if (unicode_char < 0x80U)
	{
		dest[(*pos)++] = (char)unicode_char;
	}
	else if (unicode_char < 0x800U)
	{
		dest[(*pos)++] = (char)(0xC0U | (unicode_char >> 6U));
		dest[(*pos)++] = (char)(0x80U | (unicode_char & 0x3FU));
	}
	else if (unicode_char < 0x10000U)
	{
		dest[(*pos)++] = (char)(0xE0U | (unicode_char >> 12U));
		dest[(*pos)++] = (char)(0x80U | ((unicode_char >> 6U) & 0x3FU));
		dest[(*pos)++] = (char)(0x80U | (unicode_char & 0x3FU));
	}
	else
	{
		dest[(*pos)++] = (char)(0xF0U | (unicode_char >> 18U));
		dest[(*pos)++] = (char)(0x80U | ((unicode_char >> 12U) & 0x3FU));
		dest[(*pos)++] = (char)(0x80U | ((unicode_char >> 6U) & 0x3FU));
		dest[(*pos)++] = (char)(0x80U | (unicode_char & 0x3FU));
	}
	return scanned;
}

static Expr *parse_string_literal(Context *context, Expr *left)
{
	assert(!left && "Had left hand side");
	Expr *expr_string = EXPR_NEW_TOKEN(EXPR_CONST, context->tok);
	expr_set_type(expr_string, type_compstr);

	char *str = NULL;
	size_t len = 0;

	while (TOKEN_IS(TOKEN_STRING))
	{
		char *new_string = malloc_arena(len + TOKLEN(context->tok));
		if (str) memcpy(new_string, str, len);
		const char *sourcestr = TOKSTR(context->tok);
		str = new_string;
		for (unsigned i = 1; i < TOKLEN(context->tok) - 1; i++)
		{
			if (sourcestr[i] == '\\')
			{
				i++;
				int scanned = append_esc_string_token(str, sourcestr + i, &len) - 1;
				if (scanned < -1)
				{
					SEMA_TOKEN_ERROR(context->tok, "Invalid escape in string.");
					return poisoned_expr;
				}
				i += scanned;
				continue;
			}
			str[len++] = sourcestr[i];
		}
		advance_and_verify(context, TOKEN_STRING);
	}
	if (len > UINT32_MAX)
	{
		SEMA_TOKEN_ERROR(context->tok, "String exceeded max size.");
		return poisoned_expr;
	}
	assert(str);
	str[len] = '\0';
	expr_string->const_expr.string.chars = str;
	expr_string->const_expr.string.len = (uint32_t)len;
	expr_set_type(expr_string, type_compstr);
	expr_string->const_expr.kind = TYPE_STRLIT;
	return expr_string;
}

static Expr *parse_bool(Context *context, Expr *left)
{
	assert(!left && "Had left hand side");
	Expr *number = EXPR_NEW_TOKEN(EXPR_CONST, context->tok);
	number->const_expr = (ExprConst) { .b = TOKEN_IS(TOKEN_TRUE), .kind = TYPE_BOOL };
	expr_set_type(number, type_bool);
	advance(context);
	return number;
}

static Expr *parse_null(Context *context, Expr *left)
{
	assert(!left && "Had left hand side");
	Expr *number = EXPR_NEW_TOKEN(EXPR_CONST, context->tok);
	number->const_expr.kind = TYPE_POINTER;
	expr_set_type(number, type_voidptr);
	advance(context);
	return number;
}

Expr *parse_type_compound_literal_expr_after_type(Context *context, TypeInfo *type_info)
{
	advance_and_verify(context, TOKEN_LPAREN);
	Expr *expr = expr_new(EXPR_COMPOUND_LITERAL, type_info->span);
	expr->expr_compound_literal.type_info = type_info;
	expr->expr_compound_literal.initializer = TRY_EXPR_OR(parse_initializer_list(context), poisoned_expr);
	CONSUME_OR(TOKEN_RPAREN, poisoned_expr);
	RANGE_EXTEND_PREV(expr);
	return expr;
}



/**
 * type_identifier ::= VIRTUAL? TYPE_IDENT initializer_list?
 *
 * @param left must be null.
 * @return Expr*
 */
Expr *parse_type_expression_with_path(Context *context, Path *path)
{
	TypeInfo *type;
	if (path)
	{
		type = type_info_new(TYPE_INFO_IDENTIFIER, path->span);
		type->unresolved.path = path;
		type->unresolved.name_loc = context->tok.id;
		advance_and_verify(context, TOKEN_TYPE_IDENT);
		RANGE_EXTEND_PREV(type);
		type = TRY_TYPE_OR(parse_type_with_base(context, type), poisoned_expr);
	}
	else
	{
		type = TRY_TYPE_OR(parse_type(context), poisoned_expr);
	}
	if (!type->virtual_type && TOKEN_IS(TOKEN_LPAREN) && context->next_tok.type == TOKEN_LBRACE)
	{
		return parse_type_compound_literal_expr_after_type(context, type);
	}
	Expr *expr = expr_new(EXPR_TYPEINFO, type->span);
	expr->type_expr = type;
	return expr;
}




/**
 * function_block
 *  : '{|' stmt_list '|}'
 */
static Expr* parse_expr_block(Context *context, Expr *left)
{
	assert(!left && "Had left hand side");
	Expr *expr = EXPR_NEW_TOKEN(EXPR_EXPR_BLOCK, context->tok);
	advance_and_verify(context, TOKEN_LBRAPIPE);
	while (!try_consume(context, TOKEN_RBRAPIPE))
	{
		Ast *stmt = parse_stmt(context);
		if (!ast_ok(stmt)) return poisoned_expr;
		vec_add(expr->expr_block.stmts, stmt);
	}
	RANGE_EXTEND_PREV(expr);
	return expr;
}

ParseRule rules[TOKEN_EOF + 1] = {
		[TOKEN_VIRTUAL] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_BOOL] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_CHAR] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_ICHAR] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_SHORT] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_USHORT] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_INT] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_UINT] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_LONG] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_ULONG] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_INT128] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_UINT128] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_ISIZE] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_USIZE] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_IPTR] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_UPTR] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_IPTRDIFF] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_UPTRDIFF] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_FLOAT] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_DOUBLE] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_FLOAT16] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_FLOAT128] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_VOID] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_TYPEID] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_ANYERR] = { parse_type_identifier, NULL, PREC_NONE },

		[TOKEN_ELSE] = { NULL, parse_else_expr, PREC_ELSE },
		[TOKEN_QUESTION] = { NULL, parse_ternary_expr, PREC_TERNARY },
		[TOKEN_ELVIS] = { NULL, parse_ternary_expr, PREC_TERNARY },
		[TOKEN_PLUSPLUS] = { parse_unary_expr, parse_post_unary, PREC_CALL },
		[TOKEN_MINUSMINUS] = { parse_unary_expr, parse_post_unary, PREC_CALL },
		[TOKEN_LPAREN] = { parse_grouping_expr, parse_call_expr, PREC_CALL },
		[TOKEN_LBRAPIPE] = { parse_expr_block, NULL, PREC_NONE },
		[TOKEN_TRY] = { parse_try_expr, NULL, PREC_NONE },
		[TOKEN_CATCH] = { parse_try_expr, NULL, PREC_NONE },
		[TOKEN_TRY_OLD] = { parse_try_old_expr, NULL, PREC_NONE },
		[TOKEN_CATCH_OLD] = { parse_try_old_expr, NULL, PREC_NONE },
		[TOKEN_BANGBANG] = { NULL, parse_bangbang_expr, PREC_CALL },
		[TOKEN_LBRACKET] = { NULL, parse_subscript_expr, PREC_CALL },
		[TOKEN_MINUS] = { parse_unary_expr, parse_binary, PREC_ADDITIVE },
		[TOKEN_PLUS] = { NULL, parse_binary, PREC_ADDITIVE },
		[TOKEN_DIV] = { NULL, parse_binary, PREC_MULTIPLICATIVE },
		[TOKEN_MOD] = { NULL, parse_binary, PREC_MULTIPLICATIVE },
		[TOKEN_STAR] = { parse_unary_expr, parse_binary, PREC_MULTIPLICATIVE },
		[TOKEN_DOT] = { NULL, parse_access_expr, PREC_CALL },
		[TOKEN_BANG] = { parse_unary_expr, parse_failable, PREC_UNARY },
		[TOKEN_BIT_NOT] = { parse_unary_expr, NULL, PREC_UNARY },
		[TOKEN_BIT_XOR] = { NULL, parse_binary, PREC_BIT },
		[TOKEN_BIT_OR] = { NULL, parse_binary, PREC_BIT },
		[TOKEN_AMP] = { parse_unary_expr, parse_binary, PREC_BIT },
		[TOKEN_EQEQ] = { NULL, parse_binary, PREC_RELATIONAL },
		[TOKEN_NOT_EQUAL] = { NULL, parse_binary, PREC_RELATIONAL },
		[TOKEN_GREATER] = { NULL, parse_binary, PREC_RELATIONAL },
		[TOKEN_GREATER_EQ] = { NULL, parse_binary, PREC_RELATIONAL },
		[TOKEN_LESS] = { NULL, parse_binary, PREC_RELATIONAL },
		[TOKEN_LESS_EQ] = { NULL, parse_binary, PREC_RELATIONAL },
		[TOKEN_SHL] = { NULL, parse_binary, PREC_SHIFT },
		[TOKEN_SHR] = { NULL, parse_binary, PREC_SHIFT },
		[TOKEN_TRUE] = { parse_bool, NULL, PREC_NONE },
		[TOKEN_FALSE] = { parse_bool, NULL, PREC_NONE },
		[TOKEN_NULL] = { parse_null, NULL, PREC_NONE },
		[TOKEN_INTEGER] = { parse_integer, NULL, PREC_NONE },
		[TOKEN_PLACEHOLDER] = { parse_placeholder, NULL, PREC_NONE },
		[TOKEN_CHAR_LITERAL] = { parse_char_lit, NULL, PREC_NONE },
		[TOKEN_AT] = { parse_macro_expansion, NULL, PREC_NONE },
		[TOKEN_STRING] = { parse_string_literal, NULL, PREC_NONE },
		[TOKEN_REAL] = { parse_double, NULL, PREC_NONE },
		[TOKEN_OR] = { NULL, parse_binary, PREC_OR },
		[TOKEN_AND] = { parse_unary_expr, parse_binary, PREC_AND },
		[TOKEN_EQ] = { NULL, parse_binary, PREC_ASSIGNMENT },
		[TOKEN_PLUS_ASSIGN] = { NULL, parse_binary, PREC_ASSIGNMENT },
		[TOKEN_MINUS_ASSIGN] = { NULL, parse_binary, PREC_ASSIGNMENT },
		[TOKEN_MULT_ASSIGN] = { NULL, parse_binary, PREC_ASSIGNMENT },
		[TOKEN_MOD_ASSIGN] = { NULL, parse_binary, PREC_ASSIGNMENT },
		[TOKEN_DIV_ASSIGN] = { NULL, parse_binary, PREC_ASSIGNMENT },
		[TOKEN_BIT_XOR_ASSIGN] = { NULL, parse_binary, PREC_ASSIGNMENT },
		[TOKEN_BIT_AND_ASSIGN] = { NULL, parse_binary, PREC_ASSIGNMENT },
		[TOKEN_BIT_OR_ASSIGN] = { NULL, parse_binary, PREC_ASSIGNMENT },
		[TOKEN_SHR_ASSIGN] = { NULL, parse_binary, PREC_ASSIGNMENT },
		[TOKEN_SHL_ASSIGN] = { NULL, parse_binary, PREC_ASSIGNMENT },

		[TOKEN_IDENT] = { parse_identifier_starting_expression, NULL, PREC_NONE },
		[TOKEN_TYPE_IDENT] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_CT_IDENT] = { parse_ct_ident, NULL, PREC_NONE },
		[TOKEN_CONST_IDENT] = { parse_identifier, NULL, PREC_NONE },
		[TOKEN_CT_CONST_IDENT] = { parse_ct_ident, NULL, PREC_NONE },
		[TOKEN_CT_TYPE_IDENT] = { parse_type_identifier, NULL, PREC_NONE },
		[TOKEN_HASH_IDENT] = { parse_hash_ident, NULL, PREC_NONE },
		//[TOKEN_HASH_TYPE_IDENT] = { parse_type_identifier(, NULL, PREC_NONE }

		[TOKEN_CT_SIZEOF] = { parse_ct_call, NULL, PREC_NONE },
		[TOKEN_CT_ALIGNOF] = { parse_ct_call, NULL, PREC_NONE },
		[TOKEN_CT_OFFSETOF] = { parse_ct_call, NULL, PREC_NONE },
		[TOKEN_CT_NAMEOF] = { parse_ct_call, NULL, PREC_NONE },
		[TOKEN_CT_QNAMEOF] = { parse_ct_call, NULL, PREC_NONE },
		[TOKEN_CT_TYPEOF] = { parse_typeof_expr, NULL, PREC_NONE },
};
