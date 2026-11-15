/*
 * Arithmetic over JSON numbers. Shows the optional exports: jd_describe()
 * and jd_flags (this plugin is reentrant, so calls may run in parallel).
 *
 *   curl -d '[1, 2, 3.5]' localhost:8080/math/add             -> 6.5
 *   curl 'localhost:8080/math/add?a=40&b=2'                   -> 42
 *   curl -d '{"jsonrpc":"2.0","method":"div","params":[1,0],"id":1}' localhost:8080/math
 *
 * Operands are the params array, or the values of the params object in order.
 * Integers stay integers (64 bits) as long as the result is exact, then the
 * computation goes on in floating point.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "jsondaemon.h"

#define MATH_DIVISION_BY_ZERO 1001
#define MATH_NOT_FINITE       1002

const unsigned jd_flags = JD_THREAD_SAFE;

typedef struct number {
	int is_int;
	int64_t i;
	double d;
} number;

typedef enum operation { OP_ADD, OP_SUB, OP_MUL, OP_DIV } operation;

static const struct {
	const char *name;
	operation op;
	size_t min_operands;
	const char *help;
} methods[] = {
	{"add", OP_ADD, 0, "Sum of the operands (0 without any)."},
	{"sub", OP_SUB, 1, "First operand minus all the others."},
	{"mul", OP_MUL, 0, "Product of the operands (1 without any)."},
	{"div", OP_DIV, 2, "First operand divided by all the others."},
};

static double as_double(number n)
{
	return n.is_int ? (double)n.i : n.d;
}

static int apply(operation op, number *acc, number x, jd_error *err)
{
	if (acc->is_int && x.is_int) {
		int64_t r = 0;
		int overflow = 0;

		switch (op) {
		case OP_ADD: overflow = __builtin_add_overflow(acc->i, x.i, &r); break;
		case OP_SUB: overflow = __builtin_sub_overflow(acc->i, x.i, &r); break;
		case OP_MUL: overflow = __builtin_mul_overflow(acc->i, x.i, &r); break;
		case OP_DIV:
			if (x.i == 0) {
				jd_fail(err, MATH_DIVISION_BY_ZERO, "division by zero");
				return -1;
			}
			/* Exact integer division only; INT64_MIN / -1 overflows. */
			overflow = (acc->i == INT64_MIN && x.i == -1) || acc->i % x.i != 0;
			r = overflow ? 0 : acc->i / x.i;
			break;
		}
		if (!overflow) {
			acc->i = r;
			return 0;
		}
	}

	/* Floating point from here on. */
	{
		double a = as_double(*acc), b = as_double(x);

		if (op == OP_DIV && b == 0.0) {
			jd_fail(err, MATH_DIVISION_BY_ZERO, "division by zero");
			return -1;
		}
		acc->is_int = 0;
		acc->d = op == OP_ADD ? a + b : op == OP_SUB ? a - b : op == OP_MUL ? a * b : a / b;
	}
	return 0;
}

static int to_number(json_object *value, size_t index, number *out, jd_error *err)
{
	switch (json_object_get_type(value)) {
	case json_type_int:
		out->is_int = 1;
		out->i = json_object_get_int64(value);
		out->d = 0;
		return 0;
	case json_type_double:
		out->is_int = 0;
		out->i = 0;
		out->d = json_object_get_double(value);
		return 0;
	default:
		jd_fail(err, JD_INVALID_PARAMS, "operand %zu is not a number: %s", index + 1,
		        json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN));
		return -1;
	}
}

json_object *jd_call(const char *method, json_object *params, jd_error *err)
{
	json_object *operands[256];
	size_t n = 0, m, i;
	number acc = {1, 0, 0};

	for (m = 0; m < sizeof(methods) / sizeof(methods[0]); m++)
		if (strcmp(methods[m].name, method) == 0)
			break;
	if (m == sizeof(methods) / sizeof(methods[0]))
		return jd_fail(err, JD_METHOD_NOT_FOUND, "unknown method '%s', try add, sub, mul or div", method);

	if (params == NULL) {
		n = 0;
	} else if (json_object_is_type(params, json_type_array)) {
		n = json_object_array_length(params);
		if (n > sizeof(operands) / sizeof(operands[0]))
			return jd_fail(err, JD_INVALID_PARAMS, "at most 256 operands");
		for (i = 0; i < n; i++)
			operands[i] = json_object_array_get_idx(params, i);
	} else if (json_object_is_type(params, json_type_object)) {
		json_object_object_foreach(params, key, value) {
			(void)key;
			if (n == sizeof(operands) / sizeof(operands[0]))
				return jd_fail(err, JD_INVALID_PARAMS, "at most 256 operands");
			operands[n++] = value;
		}
	} else {
		return jd_fail(err, JD_INVALID_PARAMS, "params must be an array or an object of numbers");
	}
	if (n < methods[m].min_operands)
		return jd_fail(err, JD_INVALID_PARAMS, "'%s' needs at least %zu operands", method, methods[m].min_operands);

	acc.i = methods[m].op == OP_MUL ? 1 : 0;
	for (i = 0; i < n; i++) {
		number x = {0, 0, 0};

		if (to_number(operands[i], i, &x, err) < 0)
			return NULL;
		if (i == 0 && (methods[m].op == OP_SUB || methods[m].op == OP_DIV)) {
			acc = x;
			continue;
		}
		if (apply(methods[m].op, &acc, x, err) < 0)
			return NULL;
	}

	if (acc.is_int)
		return json_object_new_int64(acc.i);
	if (!isfinite(acc.d))
		return jd_fail(err, MATH_NOT_FINITE, "the result is not a finite number");
	return json_object_new_double(acc.d);
}

const char *jd_mount(void)
{
	return "/math";
}

json_object *jd_describe(void)
{
	json_object *about = json_object_new_object(), *list = json_object_new_object();
	size_t m;

	json_object_object_add(about, "description", json_object_new_string("Arithmetic over JSON numbers."));
	json_object_object_add(about, "params",
	                       json_object_new_string("an array of numbers, or an object whose values are numbers"));
	for (m = 0; m < sizeof(methods) / sizeof(methods[0]); m++)
		json_object_object_add(list, methods[m].name, json_object_new_string(methods[m].help));
	json_object_object_add(about, "methods", list);
	return about;
}
