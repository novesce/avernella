// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Call video receive in src/call_video.c: SRTP, the RTP header, extensions
 * and padding, EVS3 and RFC 7741 descriptors, fragment acceptance and
 * picture assembly, and the PDTP screen-share decoder. Complete pictures
 * reach the VP8 decoder, and every decoded frame is read in full so that
 * AddressSanitizer checks its planes against the reported geometry.
 *
 * Input: a configuration byte, then operations. Each operation is a byte
 * whose low two bits choose it, followed by a two-byte big-endian length
 * and a body. RTP bodies are plaintext packets: the harness fixes the RTP
 * version, payload type and SSRC, then protects them with the synthetic key
 * of tests/call_video.c. Screen-share bodies are whole PDTP messages.
 */
#include "call_srtp.h"
#include "call_video.h"
#include "fuzz.h"
#include <string.h>

enum {
	VIDEO_PT = 97,
	VIDEO_SSRC = 101,
	MAX_PACKET = 4096,
};

static const guint8 key[30] = { 1, 2, 3 };

enum {
	CONFIG_GROUP = 1 << 0,
	CONFIG_STATS = 1 << 1,
};

struct view {
	struct call_video *video;
	guint frames;
};

static void display(const struct line_camera_frame *frame, gpointer data)
{
	struct view *view = data;

	g_assert_cmpuint(frame->width, >=, 1);
	g_assert_cmpuint(frame->height, >=, 1);
	g_assert_cmpuint(frame->width, <=, 4096);
	g_assert_cmpuint(frame->height, <=, 4096);
	for (guint i = 0; i < 3; i++) {
		guint w = i ? (frame->width + 1) / 2 : frame->width;
		guint h = i ? (frame->height + 1) / 2 : frame->height;

		g_assert_nonnull(frame->plane[i]);
		g_assert_cmpint(frame->stride[i], >=, (int)w);
		for (guint y = 0; y < h; y++)
			fuzz_touch(frame->plane[i] +
					   (gsize)y * frame->stride[i],
				   w);
	}
	if (view->video)
		call_video_frame_is_screen(view->video);
	view->frames++;
}

static void receive(struct view *view, srtp_t sender, const guint8 *body,
		    gsize n)
{
	guint8 packet[MAX_PACKET + SRTP_MAX_TRAILER_LEN] = { 0 };
	int size;

	n = MIN(n, MAX_PACKET - SRTP_MAX_TRAILER_LEN);
	memcpy(packet, body, n);
	size = MAX(n, 12u);
	packet[0] = 0x80 | (packet[0] & 0x3f);
	packet[1] = (packet[1] & 0x80) | VIDEO_PT;
	packet[8] = 0;
	packet[9] = 0;
	packet[10] = 0;
	packet[11] = VIDEO_SSRC;
	if (srtp_protect(sender, packet, &size) == srtp_err_status_ok)
		call_video_receive(view->video, packet, size, display, view);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	fuzz_quiet();
	g_setenv("LINE_DISABLE_GPU", "1", TRUE);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct fuzz_input in = { data, size };
	struct call_share_decoder *share = NULL;
	struct view view = { 0 };
	guint config = fuzz_byte(&in);
	srtp_t sender;

	if (config & CONFIG_STATS)
		g_setenv("LINE_GROUP_CALL_STATS", "1", TRUE);
	else
		g_unsetenv("LINE_GROUP_CALL_STATS");
	view.video =
		config & CONFIG_GROUP ?
			call_video_open_group_rx(key, VIDEO_PT, VIDEO_SSRC,
						 NULL) :
			call_video_open_rx(key, VIDEO_PT, VIDEO_SSRC, NULL);
	g_assert_nonnull(view.video);
	g_assert_true(call_srtp_open(&sender, key, ssrc_specific, VIDEO_SSRC));

	while (in.n) {
		guint op = fuzz_byte(&in);
		const guint8 *body;
		gsize n = fuzz_chunk(&in, &body);

		switch (op & 3) {
		case 0:
		case 1:
			receive(&view, sender, body, n);
			break;
		case 2:
			if (!share)
				share = call_share_decoder_new();
			g_assert_nonnull(share);
			call_share_decode(share, body, n, display, &view);
			call_share_decoder_keyframe_needed(share);
			break;
		default:
			call_video_needs_keyframe(view.video);
			call_video_authenticated_packets(view.video);
			call_video_frame_is_screen(view.video);
			call_video_report(view.video, op & 4);
			break;
		}
	}
	call_share_decoder_free(share);
	srtp_dealloc(sender);
	call_video_close(view.video);
	return 0;
}
