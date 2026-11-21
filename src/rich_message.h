/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "line.h"

/*
 * LINE rich messages (ContentType RICH = 17, the official accounts'
 * imagemap) carry MARKUP_JSON, DOWNLOAD_URL and ALT_TEXT in the message
 * metadata. The markup is the Android RichMessage model:
 *
 *   canvas  {width, height, initialScene}
 *   images  {name: {x, y, w, h, imageUrl?}}   regions of the base image
 *   actions {name: {type, text, params}}      web, postback, sendMessage,
 *                                              clipboard, transition, app
 *   scenes  {name: {draws: [{x, y, w, h, image}],
 *                   listeners: [{type: "touch", params: [x, y, w, h],
 *                                action}], video}}
 *
 * line_rich_parse validates all of it and returns a compact layout:
 *
 *   {w, h, scene, scenes: {name: {draws: [{x, y, w, h, url, sx, sy, sw,
 *    sh, whole}], areas: [{x, y, w, h, kind, value, label}]}}}
 *
 * kind is "link" (an https URL), "message" (text to send), "copy" or
 * "scene". Base images use DOWNLOAD_URL/<width>, as LINE's imagemap does;
 * the downloader falls back to DOWNLOAD_URL when the object store rejects it.
 * Returns NULL for anything malformed; *alt receives the alternative text.
 */
J *line_rich_parse(J *metadata, char **alt);

/* The base-image width the window asks for: one of LINE's sizes. */
#define LINE_RICH_IMAGE_WIDTH 700
