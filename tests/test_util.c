/* test_util.c - the buffer and drawing scales and the seats: make check */
#include <stdio.h>

#include "util.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_buffer_scale(void)
{
	CHECK(buffer_scale(1) == 1);
	CHECK(buffer_scale(2) == 2);
	CHECK(buffer_scale(16) == 16);
	CHECK(buffer_scale(17) == 16);
	/* 256 times 2^23 would pass INT_MAX */
	CHECK(buffer_scale(1 << 23) == 16);
	CHECK(buffer_scale(0x7fffffff) == 16);
	CHECK(buffer_scale(0) == 0);
	CHECK(buffer_scale(-1) == 0);
	CHECK(buffer_scale(-0x7fffffff - 1) == 0);
}

static void test_draw_scale(void)
{
	CHECK(draw_scale(100, 1) == 1);
	CHECK(draw_scale(100, 16) == 16);
	CHECK(draw_scale(2047, 16) == 16);              /* 32752 */
	CHECK(draw_scale(2048, 16) == 15);              /* 32768 is one too many */
	CHECK(draw_scale(16383, 2) == 2);
	CHECK(draw_scale(16384, 2) == 1);
	/* 900 cores, about 9000 wide: 36000 at 4, 27000 at 3 */
	CHECK(draw_scale(9000, 4) == 3);
	/* past Cairo at 1 too: stays 1, black, and small enough for an int */
	CHECK(draw_scale(41000, 16) == 1);
	CHECK(draw_scale(CAIRO_MAX_W, 1) == 1);
}

static int next_is(const struct seats *s, unsigned name, unsigned ver)
{
	unsigned n = 0, v = 0;
	return seats_next(s, &n, &v) && n == name && v == ver;
}

static void test_seats(void)
{
	struct seats s = { 0 };
	unsigned n, v;
	CHECK(!seats_next(&s, &n, &v));
	/* seat0 and seat1 at the start: seat0 is taken */
	CHECK(seats_add(&s, 10, 7) && seats_add(&s, 11, 5));
	CHECK(next_is(&s, 10, 7));
	/* other globals going leave the list alone */
	seats_remove(&s, 99);
	CHECK(s.n == 2 && next_is(&s, 10, 7));
	/* seat0 goes: seat1, there all along, is next (digitalClock's review 2026-11-20, 8) */
	seats_remove(&s, 10);
	CHECK(next_is(&s, 11, 5));
	/* a new one comes after it */
	CHECK(seats_add(&s, 12, 9));
	CHECK(next_is(&s, 11, 5));
	/* one that is not taken goes: the taken one stays first */
	seats_remove(&s, 12);
	CHECK(s.n == 1 && next_is(&s, 11, 5));
	seats_remove(&s, 11);
	CHECK(s.n == 0 && !seats_next(&s, &n, &v));
	/* the order holds when one goes from the middle */
	for (unsigned i = 0; i < 3; i++)
		seats_add(&s, 20 + i, i);
	seats_remove(&s, 21);
	seats_remove(&s, 20);
	CHECK(s.n == 1 && next_is(&s, 22, 2));
	seats_remove(&s, 22);
	/* full: one more is not kept, the others are */
	for (unsigned i = 0; i < MAX_SEATS; i++)
		CHECK(seats_add(&s, 30 + i, 1));
	CHECK(!seats_add(&s, 99, 1) && s.n == MAX_SEATS);
	seats_remove(&s, 99);
	CHECK(s.n == MAX_SEATS);
	for (unsigned i = 0; i < MAX_SEATS; i++) {
		CHECK(next_is(&s, 30 + i, 1));
		seats_remove(&s, 30 + i);
	}
	CHECK(s.n == 0);
}

int main(void)
{
	test_buffer_scale();
	test_draw_scale();
	test_seats();
	printf("test_util: %d/%d\n", checks - failures, checks);
	return failures != 0;
}
