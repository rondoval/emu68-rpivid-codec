/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dpb - the decoded picture buffer's output process on hand-made picture orders (developer-guide.md section 4.3).
 *
 * Each case stores pictures by POC in a decoding order, runs C.5.2 around
 * each as stream.c does, takes the output after every picture as the
 * session does, and holds the pictures shown to the order and the set the
 * standard gives: reordering alone; the latency count of C.5.2.3, which
 * counts only for a current picture that is output and only the pictures
 * after it in output order (both orders were found by searching for the
 * ones that counting every waiting picture on every store shows wrongly);
 * an IRAP that starts afresh with and without NoOutputOfPriorPicsFlag.
 */
#include <stdio.h>
#include <string.h>

#include "dpb.h"
#include "host_check.h"

#define PICTURES_MAX 8 /* the most pictures a case stores */

/* A case: pictures in decoding order, and what they must show */
struct order_case
{
	const char *name;               /* what the case holds to */
	u32 count;                      /* the pictures */
	s32 poc[PICTURES_MAX];          /* each one's PicOrderCntVal, in decoding order */
	BOOL hidden[PICTURES_MAX];      /* PicOutputFlag 0: the picture is not shown */
	u32 reorder;                    /* sps_max_num_reorder_pics */
	u32 latency_plus1;              /* sps_max_latency_increase_plus1 */
	s32 shown[PICTURES_MAX];        /* the POCs shown, in order */
	u32 shown_count;                /* how many */
};

static const struct order_case order_cases[] = {
	{"reordering alone", 5, {0, 4, 2, 1, 3}, {0}, 2, 0, {0, 1, 2, 3, 4}, 5},
	{"latency counted only for the pictures after the current one", 5, {0, 3, 1, 4, 2}, {0}, 2, 1, {0, 1, 2, 3, 4}, 5},
	{"latency not counted for a picture that is not shown", 4, {0, 3, 1, 2}, {FALSE, FALSE, TRUE, FALSE}, 1, 1, {0, 2, 3}, 3},
};

static struct rp_dpb dpb;

/**
 * take_output() - The pictures bumped out, as the session takes them: their POCs appended to a list.
 * @poc_of: each picture's POC, by number
 * @shown:  the list
 * @count:  its length, updated
 */
static void take_output(const s32 *poc_of, s32 *shown, u32 *count)
{
	for (;;)
	{
		u32 number = rp_dpb_output_next(&dpb);
		if (number == 0)
			return;
		if (*count < PICTURES_MAX)
			shown[*count] = poc_of[number];
		(*count)++;
	}
}

/**
 * sps_for() - An SPS that holds the buffer only to its reordering and latency limits.
 * @sps:           filled in
 * @reorder:       sps_max_num_reorder_pics
 * @latency_plus1: sps_max_latency_increase_plus1
 */
static void sps_for(struct rp_sps *sps, u32 reorder, u32 latency_plus1)
{
	memset(sps, 0, sizeof(*sps));
	sps->max_dec_pic_buffering = RP_DPB_SIZE_MAX; /* room for every picture of a case: no bump for a full buffer */
	sps->max_num_reorder = reorder;
	sps->max_latency_increase_plus1 = latency_plus1;
}

/**
 * check_order() - A case's pictures stored in their decoding order, and shown as the standard says.
 * @c: the case
 */
static void check_order(const struct order_case *c)
{
	static struct rp_sps sps;
	s32 poc_of[PICTURES_MAX + 1], shown[PICTURES_MAX];
	u32 count = 0;
	sps_for(&sps, c->reorder, c->latency_plus1);
	memset(&dpb, 0, sizeof(dpb));
	for (u32 i = 0; i < c->count; i++)
	{
		poc_of[i + 1] = c->poc[i];
		rp_dpb_before_picture(&dpb, &sps, FALSE, FALSE);
		CHECK(rp_dpb_store(&dpb, &sps, i + 1, c->poc[i], !c->hidden[i]), "%s: picture %u not stored", c->name, i + 1);
		take_output(poc_of, shown, &count);
	}
	rp_dpb_bump_all(&dpb);
	take_output(poc_of, shown, &count);
	CHECK(count == c->shown_count && memcmp(shown, c->shown, count * sizeof(shown[0])) == 0, "%s: %u pictures shown, POC %d %d %d...",
	      c->name, count, count > 0 ? (int)shown[0] : -1, count > 1 ? (int)shown[1] : -1, count > 2 ? (int)shown[2] : -1);
}

/**
 * check_irap() - An IRAP that starts afresh: the pictures waiting shown first, or thrown away with NoOutputOfPriorPicsFlag.
 * @no_output_of_prior: NoOutputOfPriorPicsFlag
 */
static void check_irap(BOOL no_output_of_prior)
{
	static struct rp_sps sps;
	s32 poc_of[PICTURES_MAX + 1] = {0, 0, 4, 2}, shown[PICTURES_MAX];
	u32 count = 0;
	sps_for(&sps, 2, 0);
	memset(&dpb, 0, sizeof(dpb));
	for (u32 number = 1; number <= 3; number++)
	{
		rp_dpb_before_picture(&dpb, &sps, FALSE, FALSE);
		rp_dpb_store(&dpb, &sps, number, poc_of[number], TRUE);
		take_output(poc_of, shown, &count);
	}
	CHECK(count == 1 && rp_dpb_holds(&dpb, 2) && rp_dpb_holds(&dpb, 3), "before the IRAP: POC 0 shown, 2 and 4 waiting");
	rp_dpb_unmark_all(&dpb); /* 8.3.2 for an IRAP with NoRaslOutputFlag */
	rp_dpb_before_picture(&dpb, &sps, TRUE, no_output_of_prior);
	take_output(poc_of, shown, &count);
	if (no_output_of_prior)
		CHECK(count == 1, "an IRAP with NoOutputOfPriorPicsFlag shows nothing waiting: %u shown", count);
	else
		CHECK(count == 3 && shown[1] == 2 && shown[2] == 4, "an IRAP without the flag shows the waiting pictures first, in POC order");
	CHECK(!rp_dpb_holds(&dpb, 1) && !rp_dpb_holds(&dpb, 2) && !rp_dpb_holds(&dpb, 3), "the IRAP leaves the buffer empty");
}

/**
 * main() - Run the output process's checks.
 *
 * Return: 0 when all passed, 1 when not.
 */
int main(void)
{
	for (u32 i = 0; i < sizeof(order_cases) / sizeof(order_cases[0]); i++)
		check_order(&order_cases[i]);
	check_irap(FALSE);
	check_irap(TRUE);
	printf("test_dpb: %s\n", failures ? "FAIL" : "ok");
	return failures ? 1 : 0;
}
