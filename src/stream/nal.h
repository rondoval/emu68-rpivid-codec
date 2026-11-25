/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nal.h - NAL unit types and the Annex B scanner (docs/architecture.md section 6.1).
 *
 * An Annex B stream is a run of NAL units, each after a start code: 00 00 01,
 * maybe with more zero bytes before it.  The scanner finds the units in the
 * packets a player sends.  rp_nal_packet() hands it a packet, and each
 * rp_nal_next() returns one event (a unit starting, a span of its bytes, its
 * end) until the packet is used up.  A unit may run on into the next packet;
 * as only the next start code would end it, rp_nal_finish() ends the last one
 * where a frame or the stream ends.
 *
 * Nothing is copied and nothing is called back: a span points into the
 * packet, and the session copies a VCL unit's spans into the picture's
 * bitstream buffer and a non-VCL unit's into its side buffer, then parses the
 * unit at its end.
 *
 * The rest is the standard's vocabulary for the units: the type numbers of
 * Table 7-1, and small tests on a type or a header that the stream and
 * session layers use to tell pictures, slices and parameter sets apart.
 */
#ifndef RP_NAL_H
#define RP_NAL_H

#include <exec/types.h>
#include <types.h>

/* NAL unit types (7.4.2.2, Table 7-1); the ones not named here are reserved or unspecified */
#define NAL_TRAIL_N 0
#define NAL_TRAIL_R 1
#define NAL_TSA_N 2
#define NAL_TSA_R 3
#define NAL_STSA_N 4
#define NAL_STSA_R 5
#define NAL_RADL_N 6
#define NAL_RADL_R 7
#define NAL_RASL_N 8
#define NAL_RASL_R 9
#define NAL_BLA_W_LP 16
#define NAL_BLA_W_RADL 17
#define NAL_BLA_N_LP 18
#define NAL_IDR_W_RADL 19
#define NAL_IDR_N_LP 20
#define NAL_CRA_NUT 21
#define NAL_RSV_IRAP_VCL23 23
#define NAL_VCL_TYPES 32 /* the VCL types, the units that carry slice data, are 0 to 31 */
#define NAL_VPS_NUT 32
#define NAL_SPS_NUT 33
#define NAL_PPS_NUT 34
#define NAL_AUD_NUT 35
#define NAL_EOS_NUT 36
#define NAL_EOB_NUT 37
#define NAL_FD_NUT 38
#define NAL_PREFIX_SEI_NUT 39
#define NAL_SUFFIX_SEI_NUT 40
#define NAL_RSV_NVCL41 41 /* reserved non-VCL types that may start an access unit: 41 to 44 */
#define NAL_RSV_NVCL44 44
#define NAL_UNSPEC48 48 /* unspecified types that may start an access unit: 48 to 55 */
#define NAL_UNSPEC55 55

/*
 * The header, nal_unit_header() (7.3.1.2): two bytes, most significant bit
 * first, the field widths in bits in brackets:
 *
 *   byte 0: forbidden_zero_bit (1), nal_unit_type (6), nuh_layer_id's top bit (1)
 *   byte 1: nuh_layer_id's other bits (5), nuh_temporal_id_plus1 (3)
 */
#define NAL_HEADER_BYTES 2
#define NAL_BASE_LAYER 0                  /* the nuh_layer_id of the only layer decoded: a decoder ignores units of any other (7.4.2.2) */
#define NAL_FIRST_SLICE_SEGMENT_BIT 0x80u /* first_slice_segment_in_pic_flag: the first bit after a VCL unit's header */

/**
 * nal_is_vcl() - Whether a NAL unit type is a VCL one, a unit that carries slice data.
 * @type: the NAL unit type
 *
 * Return: TRUE for the types 0 to 31, reserved ones included.
 */
static inline BOOL nal_is_vcl(u32 type)
{
	return type < NAL_VCL_TYPES;
}

/**
 * nal_is_irap() - Whether a NAL unit type is an IRAP picture's, one decodable without any picture before it.
 * @type: the NAL unit type
 *
 * IRAP is intra random access point: a BLA, an IDR, a CRA, or the two
 * reserved IRAP types 22 and 23.
 *
 * Return: TRUE for the types 16 to 23.
 */
static inline BOOL nal_is_irap(u32 type)
{
	return type >= NAL_BLA_W_LP && type <= NAL_RSV_IRAP_VCL23;
}

/**
 * nal_is_idr() - Whether a NAL unit type is an IDR picture's.
 * @type: the NAL unit type
 *
 * IDR is instantaneous decoding refresh: an IRAP after which no picture
 * refers to one before it, and the POC starts again at 0.
 *
 * Return: TRUE for IDR_W_RADL and IDR_N_LP.
 */
static inline BOOL nal_is_idr(u32 type)
{
	return type == NAL_IDR_W_RADL || type == NAL_IDR_N_LP;
}

/**
 * nal_is_bla() - Whether a NAL unit type is a broken link access picture's.
 * @type: the NAL unit type
 *
 * A BLA is an IRAP where a stream was spliced: the pictures after it in
 * decoding order that referred to pictures before it (its RASL pictures)
 * cannot be decoded.
 *
 * Return: TRUE for BLA_W_LP, BLA_W_RADL and BLA_N_LP.
 */
static inline BOOL nal_is_bla(u32 type)
{
	return type >= NAL_BLA_W_LP && type <= NAL_BLA_N_LP;
}

/**
 * nal_is_rasl() - Whether a NAL unit type is a random access skipped leading picture's.
 * @type: the NAL unit type
 *
 * A RASL picture comes after its IRAP in decoding order and before it in
 * output order, and refers to pictures before the IRAP: when decoding starts
 * at that IRAP, it is skipped.
 *
 * Return: TRUE for RASL_N and RASL_R.
 */
static inline BOOL nal_is_rasl(u32 type)
{
	return type == NAL_RASL_N || type == NAL_RASL_R;
}

/**
 * nal_is_radl() - Whether a NAL unit type is a random access decodable leading picture's.
 * @type: the NAL unit type
 *
 * A RADL picture comes after its IRAP in decoding order and before it in
 * output order, like a RASL one, but refers to no picture before the IRAP:
 * it is decoded even when decoding starts at that IRAP.
 *
 * Return: TRUE for RADL_N and RADL_R.
 */
static inline BOOL nal_is_radl(u32 type)
{
	return type == NAL_RADL_N || type == NAL_RADL_R;
}

/**
 * nal_is_sub_layer_non_reference() - Whether a NAL unit type is a sub-layer non-reference picture's.
 * @type: the NAL unit type
 *
 * An SLNR picture, in the standard's abbreviation: no later picture of the
 * same sub-layer (the same TemporalId) refers to it, and the POC derivation
 * passes over it when it picks the
 * previous picture to take the POC's high bits from (8.3.1).  These are the
 * even types below 16 (7.4.2.2): TRAIL_N, TSA_N, STSA_N, RADL_N, RASL_N and
 * the reserved types 10, 12 and 14.
 *
 * Return: TRUE for those types.
 */
static inline BOOL nal_is_sub_layer_non_reference(u32 type)
{
	return type < NAL_BLA_W_LP && (type & 1u) == 0;
}

/**
 * nal_is_slice_segment() - Whether a NAL unit type is a slice segment a decoder takes.
 * @type: the NAL unit type
 *
 * The VCL types the standard defines; a decoder ignores the reserved ones
 * (10 to 15, 22 and 23, 24 to 31).
 *
 * Return: TRUE for the types 0 to 9 (up to RASL_R) and 16 to 21 (up to CRA).
 */
static inline BOOL nal_is_slice_segment(u32 type)
{
	return type <= NAL_RASL_R || (type >= NAL_BLA_W_LP && type <= NAL_CRA_NUT);
}

/**
 * nal_is_parameter_set() - Whether a NAL unit type is a parameter set: a VPS, an SPS or a PPS.
 * @type: the NAL unit type
 *
 * Return: TRUE for the types 32 to 34.
 */
static inline BOOL nal_is_parameter_set(u32 type)
{
	return type >= NAL_VPS_NUT && type <= NAL_PPS_NUT;
}

/**
 * nal_type_ends_picture() - Whether a non-VCL NAL unit type ends the picture being taken.
 * @type: the NAL unit type, not a VCL one
 *
 * None of a picture's slices can come after a unit of these types.  They are
 * the types 7.4.2.4.4 lets start an access unit (a VPS, an SPS, a PPS, an
 * access unit delimiter, a prefix SEI, and the reserved and unspecified
 * types 41 to 44 and 48 to 55) and the two that end one, an end of sequence
 * and an end of bitstream.  The other non-VCL types, a suffix SEI and filler
 * data among them, belong to the picture before them and end nothing.
 *
 * Return: TRUE when the picture being taken ends before the unit.
 */
static inline BOOL nal_type_ends_picture(u32 type)
{
	return (type >= NAL_VPS_NUT && type <= NAL_EOB_NUT) || type == NAL_PREFIX_SEI_NUT ||
	       (type >= NAL_RSV_NVCL41 && type <= NAL_RSV_NVCL44) || (type >= NAL_UNSPEC48 && type <= NAL_UNSPEC55);
}

/**
 * nal_starts_picture() - Whether a slice segment NAL unit is its picture's first.
 * @nal: the unit, header included
 * @len: its length in bytes
 *
 * The first bit after the header is first_slice_segment_in_pic_flag (7.3.6.1).
 *
 * Return: TRUE when the flag is set; FALSE when it is not, or when the unit is
 * too short to hold it.
 */
static inline BOOL nal_starts_picture(const u8 *nal, u32 len)
{
	return len > NAL_HEADER_BYTES && (nal[NAL_HEADER_BYTES] & NAL_FIRST_SLICE_SEGMENT_BIT) != 0;
}

/**
 * nal_header_type() - The nal_unit_type of a NAL unit header: byte 0's six bits after the forbidden bit.
 * @header: the header's two bytes
 *
 * Return: the type, 0 to 63.
 */
static inline u32 nal_header_type(const u8 *header)
{
	return (header[0] >> 1) & 0x3fu;
}

/**
 * nal_header_layer() - The nuh_layer_id of a NAL unit header: byte 0's last bit, then byte 1's top five.
 * @header: the header's two bytes
 *
 * Return: the layer, 0 to 63.
 */
static inline u32 nal_header_layer(const u8 *header)
{
	return ((header[0] & 1u) << 5) | (header[1] >> 3);
}

/**
 * nal_header_temporal_id() - The TemporalId of a NAL unit header: byte 1's last three bits, less one.
 * @header: the header's two bytes
 *
 * The field is nuh_temporal_id_plus1, which the standard forbids to be 0; a
 * damaged header with a 0 there reads as TemporalId 0.
 *
 * Return: the TemporalId, 0 to 6.
 */
static inline u32 nal_header_temporal_id(const u8 *header)
{
	u32 plus1 = header[1] & 7u;
	return plus1 != 0 ? plus1 - 1u : 0;
}

/*
 * What rp_nal_next() and rp_nal_finish() return.  The span of RP_NAL_START
 * and RP_NAL_BYTES (the scanner's data and len) is good until the next call:
 * it points into the packet, which the caller keeps until it is used up, or
 * into the scanner itself.
 */
enum rp_nal_event
{
	RP_NAL_NONE,  /* nothing more: the packet is used up, or no unit was open to finish */
	RP_NAL_START, /* a unit begins: type, layer and temporal_id are set; the span is its two header bytes */
	RP_NAL_BYTES, /* the next span of the current unit's bytes */
	RP_NAL_END,   /* the current unit is complete */
};

/* The scanner: rp_nal_init() starts it at the start of a stream, rp_nal_packet() gives it bytes */
struct rp_nal
{
	/* kept from one packet to the next */
	BOOL in_unit;                /* a unit has started and not ended yet */
	BOOL expect_header;          /* a start code was just seen: the next bytes are a unit's header */
	u32 zero_run;                /* zero bytes seen, not given out: the next start code's or the unit's own, as the byte after them tells */
	u32 owed_zeros;              /* zero bytes known to be the unit's, still to be given out, two per RP_NAL_BYTES */
	u32 header_len;              /* the header bytes seen so far, 0 to NAL_HEADER_BYTES: a header may straddle two packets */
	u8 header[NAL_HEADER_BYTES]; /* the header bytes seen, given out as the unit's first span */
	/* the current unit, from its header */
	u32 type;        /* nal_unit_type */
	u32 layer;       /* nuh_layer_id */
	u32 temporal_id; /* TemporalId: nuh_temporal_id_plus1 less one */
	/* the span of the last RP_NAL_START or RP_NAL_BYTES */
	const u8 *data; /* its first byte: in the packet, or in the scanner's own header or zeros */
	u32 len;        /* its length in bytes */
	/* the packet being scanned */
	const u8 *packet; /* its bytes, from rp_nal_packet() */
	u32 packet_len;   /* its length in bytes */
	u32 packet_pos;   /* the index of the next byte to look at */
};

void rp_nal_init(struct rp_nal *n);
void rp_nal_packet(struct rp_nal *n, const u8 *data, u32 len);
enum rp_nal_event rp_nal_next(struct rp_nal *n);
enum rp_nal_event rp_nal_finish(struct rp_nal *n);

#endif /* RP_NAL_H */
