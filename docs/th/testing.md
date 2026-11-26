# การทดสอบ

[สารบัญ](README.md) · ก่อนหน้า: [การโทร](calls.md) · ถัดไป: [การมีส่วนร่วมพัฒนา](contributing.md)

เทสต์อัตโนมัติทั้งหมดรัน **ออฟไลน์** ไม่มีตัวไหนคุยกับ LINE และไม่ต้องใช้บัญชี ใช้บัญชี กุญแจ รูป เสียง และวิดีโอที่สังเคราะห์ขึ้น และแทนที่เครือข่ายหรือการ์ดเสียงที่ขอบของโมดูลที่กำลังทดสอบ

## การรันเทสต์

```sh
make check                      # build และรันทุกตัว
make build/test-liff            # build ตัวเดียว
./build/test-liff               # รันตัวนั้น
./build/test-sealing -p /sealing/authenticated-history-cache   # รันเคสเดียว
./build/test-sealing -l         # แสดงรายการเคส
./build/test-sealing --verbose  # พิมพ์ชื่อแต่ละเคสขณะรัน
```

`make check` รันเทสต์ทีละตัวและหยุดเมื่อเจอตัวแรกที่ล้มเหลว เทสต์ใช้ [เฟรมเวิร์กทดสอบของ GLib](https://docs.gtk.org/glib/testing.html) ตัวเลือกทั้งหมดของ GLib จึงใช้ได้ รวมถึง `-m perf` สำหรับเคสวัดประสิทธิภาพ (`./build/test-sealing -m perf -p /sealing/authenticated-history-cache`)

บางเทสต์ต้องใช้เครื่องมือเพิ่มตอนรัน

- FFmpeg (`ffmpeg`, `ffprobe`) สำหรับ `test-video-record` และ `test-video-play`
- ไดรเวอร์วิดีโอ dummy ของ SDL สำหรับเทสต์ UI (เทสต์ตั้งค่าเอง)

## วิดีโอกลุ่มและการแชร์หน้าจอ

รันจากโฟลเดอร์โปรเจกต์โดยระบุไฟล์ **APKM รุ่น 26.16.0** (รหัสรุ่น `261600106` พร้อม split สำหรับ ARM64) หรือ **base APK รุ่น 26.18.2** หากต้องการเทียบแพ็กเก็ตกับไลบรารีเนทีฟของ Android ให้ใช้ APKM และติดตั้ง `llvm-objdump` จาก LLVM

```sh
make check-group-media LINE_APK="/path/to/LINE-26.16.0.apkm"
make check-group-media LINE_APK="/path/to/LINE-26.18.2.apk"
```

เครื่องมือตรวจ SHA-256 และคำสั่ง DEX สำหรับกล้อง การเลือกผู้เข้าร่วม และ presentation ของแต่ละรุ่น สำหรับ APKM เครื่องมือยังตรวจรูปแบบแพ็กเก็ตจากไลบรารี ARM64 เทียบกับแพ็กเก็ตกล้องและหน้าจอที่ Avernella สร้างจากภาพสังเคราะห์ จากนั้นชุดทดสอบใช้รีเลย์ในเครื่องเพื่อตรวจการส่งและรับภาพ การเปลี่ยนแหล่งภาพ การคืนสถานะกล้อง และการหยุดแชร์

หากติดตั้งโมดูล Python `unicorn` ไว้ หลังจากสร้างชุดทดสอบด้วยคำสั่งด้านบนแล้ว สามารถรันโค้ด ARM64 ของ Android เพื่อตรวจว่าตัวอ่านและตัวเขียนของ LINE ตีความคำขอรับภาพที่ Avernella ส่งตรงกัน

```sh
python3 tests/group_media_apk.py --execute-presentation-codec "/path/to/LINE-26.16.0.apkm"
```

หากต้องการตรวจว่าภาพถึงโทรศัพท์จริง ให้เปิด Avernella รุ่นที่เพิ่งสร้าง เริ่มวิดีโอคอลกลุ่มกับผู้ใช้ Android สองคนที่ยินดีทดสอบ แล้วรัน `python3 tests/group_media_live.py` ในหน้าต่างเทอร์มินัลที่จะแชร์ ชุดทดสอบในเครื่องส่งภาพ VP8 ผ่านช่องข้อมูลของสายกลุ่มที่เข้ารหัส ตรวจค่าพิกเซลหลังถอดรหัส และตรวจว่าภาพที่มาช้าหลังหยุดแชร์จะไม่ทำให้ช่องเปิดขึ้นอีก หากต้องการยืนยันกับเครื่องจริง ให้เพิ่ม `--native-presentation` แล้วแชร์หน้าจอจาก Android เลื่อนหน้าจอและหยุดแชร์ เพื่อตรวจว่าช่องแยกบน Linux แสดงภาพเคลื่อนไหวและปิดตามผู้แชร์

## เทสต์ที่มี

แต่ละ `tests/<name>.c` build เป็น `build/test-<name>` (มีบางตัวใช้ชื่อเดิมตามประวัติ ดูตารางด้านล่าง) กฎของแต่ละเทสต์ใน [Makefile](../../Makefile) ระบุว่าลิงก์ object ตัวไหนบ้าง ทำให้เทสต์เล็กและเห็นชัดว่าพึ่งพาอะไร

| ไบนารีของเทสต์ | ซอร์ส | ครอบคลุม |
| -------------- | ----- | -------- |
| `test-thrift` | `thrift.c` | การเข้ารหัสและถอดรหัสแบบ Compact และ Binary ขีดจำกัด อินพุตผิดรูป |
| `test-transport` | `transport.c` | เฮดเดอร์ การหมุนเวียนโทเค็น การต่ออายุเมื่อได้รหัส 119 การปฏิเสธการต่ออายุ `Retry-After` (ห่อ libcurl) |
| `test-state` | `state.c` | การโหลดและบันทึกเซสชัน สิทธิ์ ขีดจำกัดขนาด การจองลำดับ การย้ายจากชื่อเก่า |
| `test-pair` | `pair.c` | ขั้นตอนการจับคู่ด้วย QR เส้นทาง PIN การนำเข้ากุญแจล้มเหลว |
| `test-password-login` | `password_login.c` | การลงชื่อด้วยอีเมล: การห่อ RSA ความลับจาก PIN verifier และการยืนยัน |
| `test-sealing` | `sealing.c` | การนำเข้าและตรวจ key chain ข้อความตัวต่อตัว v1 และ v2 กลุ่ม กุญแจสองรุ่น ID ไม่ตรง การยืนยันล้มเหลว แคชความลับ |
| `test-messaging` | `messaging.c` | รายการแชต แถวประวัติ การส่ง การตอบกลับ การเมนชัน รีแอ็กชัน การแจ้งอ่าน ประกาศ |
| `test-emoji-message` | `emoji_message.c` | อีโมจิ LINE ที่ปิดผนึกในข้อความ รวมถึงภาษาไทยและ Unicode นอก BMP |
| `test-media` | `media.c` | ซองของไฟล์แนบ ภาพตัวอย่าง object hash ของวิดีโอ ชื่อไฟล์ การอัปโหลดที่ถูกปฏิเสธ ข้อมูลเสีย การเปลี่ยนป้าย metadata |
| `test-obs` | `obs_http.c` | คำขอและเฮดเดอร์ของ OBS (ห่อ libcurl) |
| `test-media-io` | `media_io.c` | lease การปิดผนึก การส่งเฟรม descriptor ของ lease ที่ส่งไปพร้อมบรรทัด JSON (รวมถึงเมื่ออ่านทีละ 1 ไบต์) worker ที่เป็น non-dumpable |
| `test-gpu-health` | `gpu_health.c` | สถานะของ GPU NVIDIA บนโครงสร้าง sysfs จำลอง (ปกติ ไม่มีไดรเวอร์ ไม่มีอุปกรณ์แสดงผล ส่งต่อให้ VM GPU หายไป ไม่มีไดรเวอร์ NVIDIA) การสลับตอนเริ่มแอป และค่าไดรเวอร์ Vulkan ที่ผู้ใช้ตั้งเอง |
| `test-udp-batch` | `udp_batch.c` | `sendto`, `sendmmsg`, `UDP_SEGMENT`, `recvfrom` และ `recvmmsg` ผ่าน loopback ส่งดาตาแกรมเดียวกันตามลำดับเดิม การถอยกลับเมื่อเคอร์เนลปฏิเสธ `UDP_SEGMENT` และภายใต้ตัวกรอง seccomp ที่ปฏิเสธ `sendmmsg` กับ `recvmmsg` เครื่องหมาย DSCP ที่อ่านกลับด้วย `IP_RECVTOS` และเวลา CPU ต่อดาตาแกรมด้วย `-m perf` |
| `test-sandbox` | `sandbox.c` | ในโปรเซสลูก: สิ่งที่แซนด์บ็อกซ์ของ worker ปฏิเสธ (exec, namespace, ptrace, io_uring, clone3, BPF, ioctl ที่พิมพ์ใส่เทอร์มินัล รวมถึงเมื่อตั้งบิตสูงไว้, การรอรับ TCP, สัญญาณไปข้างนอก) การครอบคลุมเธรดที่เริ่มไว้ก่อน การที่เธรด fork, memfd, UDP แบบรวมชุด และ TCP ขาออกยังใช้ได้ `LINE_DISABLE_SANDBOX=1` และการใช้ซ้ำสองครั้ง |
| `test-friends` | `friends.c` | การค้นหา การเพิ่ม รายชื่อและการแบ่งหน้า รายการโปรด ชื่อ การซ่อนและบล็อก คำขอ การ์ด |
| `test-groups` | `groups.c` | การสร้าง การตรวจก่อนเชิญ สมาชิก คำเชิญ การเปลี่ยนชื่อ ลิงก์ การออกจากกลุ่ม |
| `test-stickers` | `stickers.c` | แค็ตตาล็อก สิทธิ์การเป็นเจ้าของ URL ของภาพ การส่ง |
| `test-emoji` | `emoji.c` | แค็ตตาล็อก ดัชนีชุด manifest นโยบายการดึงภาพ |
| `test-shared-content` | `shared_content.c` | อัลบั้ม โน้ต channel token (ห่อ libcurl) |
| `test-liff` | `liff.c` | `issueLiffView` URL สำหรับเปิด หน้าขอความยินยอมและหน้าสำรอง |
| `test-rich-message` | `rich_message.c` | การแยกวิเคราะห์ markup ของ imagemap |
| `test-flex-asset` | `flex_asset.c` | โฮสต์ที่อนุญาต การตรวจที่อยู่สาธารณะ redirect (ห่อ libcurl) |
| `test-talk-sync` | `talk_sync.c` | ตัวชี้ตำแหน่ง การแบ่งหน้า full และ partial resync |
| `test-live-sync` | `live_sync.c` | เธรด long poll การรวมชุด การหมุนเวียนโทเค็น และการกู้คืนการเชื่อมต่อเมื่อ revision เท่าเดิม |
| `test-ui-chat-refresh` | `ui_chat_refresh.c` | ใช้หน้าต่างแชทแบบไม่แสดงผลกับ worker จำลอง ทดสอบการรีเฟรชอัตโนมัติและการกดรีเฟรช ตรวจว่าข้อความร่างและหน้าข้อความย้อนหลังยังคงอยู่เมื่อเชื่อมต่ออีกครั้ง |
| `test-ui-refresh` | `ui_refresh.c` | ลองโหลดรูป Flex ที่เคยล้มเหลว ตรวจพิกเซลของรูปที่ถอดรหัสแล้ว และทดสอบทั้งข้อผิดพลาดที่มาช้ากับไฟล์รูปที่เสียหาย |
| `test-call` | `call.c` | การโทรครบสายกับ fixture relay ในเครื่อง (ห่อเสียงของ SDL) |
| `test-call-video` | `call_video.c` | VP8 ผ่าน EVS3 และ SRTP การสูญหาย ลำดับสลับ การเล่นซ้ำ keyframe descriptor |
| `test-screencast-portal` | `screencast.c` | ทดสอบการเลือกพอร์ทัล การปฏิเสธสิทธิ์ การยกเลิก และเงื่อนไขที่จะใช้ตัวเลือกจอของ Cinnamon ผ่าน D-Bus แยกสำหรับการทดสอบ |
| `test-screencast-cinnamon` | `screencast_cinnamon.c` | การเลือกจอ การเปลี่ยนจอระหว่างเลือก การล็อกหน้าจอ และการปิดเซสชันแชร์หน้าจอ |
| `test-monitor-picker` | `ui_monitor_picker.c` | การเลือกด้วยเมาส์และแป้นพิมพ์ การยืนยันก่อนแชร์ การยกเลิก และการปิดตัวเลือกจอเมื่อวางสาย |
| `test-audio` | `call_audio.c` | การจัดเฟรม Opus และ EAS2 การเล่นผ่าน jitter buffer (ห่อ `SDL_QueueAudio`) |
| `test-call-net` | `call_video.c`, `call_audio.c` | วิดีโอและเสียงผ่านเครือข่ายที่ไม่ดีแบบจำลองบนนาฬิกาเสมือน: การสูญหายเป็นช่วงแบบ Gilbert-Elliott ความหน่วง ความหน่วงไม่สม่ำเสมอ การสลับลำดับ คิวที่จำกัดแบนด์วิดท์ การขาดหาย และการค้างของ Wi-Fi ล้มเหลวเมื่อเฟรมเรต เวลาภาพค้าง screen PSNR เวลาฟื้นตัว ความหน่วงจากการถ่ายถึงหน้าจอ ช่วงเงียบ หรือความหน่วงการเล่นแย่กว่าเกณฑ์ของแต่ละสถานการณ์ ยังจำลองเธรดการโทรที่ไม่ว่างเพราะการเข้ารหัส และอัปลิงก์ Wi-Fi ที่อ่อนซึ่งเห็นผ่านคิวส่งของเคอร์เนล แล้วเทียบแต่ละกรณีกับโค้ดก่อนหน้า `--verbose` พิมพ์ค่าที่วัดได้ ([เครือข่ายที่ไม่ดี](calls.md#8-เครือข่ายที่ไม่ดี)) |
| `test-denoise` | `call_denoise.c` | RNNoise กับเสียงสังเคราะห์ |
| `test-video-record` | `video_record.c` | อัดภาพและเสียงสังเคราะห์ ตรวจด้วย ffprobe การยกเลิก การเก็บกวาด |
| `test-video-play` | `video_play.c` | การจัดการโปรเซสเล่นวิดีโอ |
| `test-profile-image` | `profile_image.c` | การครอป การย่อ ความโปร่งใส ผลลัพธ์ JPEG |
| `test-clipboard-paste` | `clipboard_paste.c` | การเตรียมรูปที่วาง |
| `test-qr-scan` | `qr_scan.c` | การโหลด ZBar และการถอดรหัส (ห่อ `dlopen`) |
| `test-chat-lists` | `chat_lists.c` | โฟลเดอร์ หมุด การปิดเสียง ปุ่มลัด ความปลอดภัยของไฟล์ |
| `test-chiptune` | `chiptune.c` | ชิปเสียงและเสียงทุกตัว |
| `test-ui-motion` | `ui_motion.c` | นาฬิกาเฟรม การเลื่อนและเป้าหมายการเลื่อน ความลึกของการกดปุ่ม เส้นโค้งการ glide ทิศทางล้อเมาส์ และการรวมเศษแถวจากทัชแพด รวมถึงจังหวะที่ 60, 144 และ 240 Hz |
| `test-ui-pixels` | `ui_pixels.c` | premultiply คณิตของการคัดลอกตอนเลื่อน |
| `test-ui-apng` | `ui_apng.c` | เฟรมและจังหวะของ PNG เคลื่อนไหว |
| `test-ui-emoji` | `ui_emoji.c` | โทเค็นในฉบับร่างและช่วงที่ส่งออก |
| `test-ui-flex` | `ui_flex.c` | การจัดวาง Flex |
| `test-ui-friends` | `ui_friends.c` | พฤติกรรมหน้าจอเพื่อน |
| `test-ui-tray` | `ui_tray.c` | คุณสมบัติและเมนูของไอเทมใน tray |
| `test-ui-read` | `ui_read.c` | กฎการแจ้งอ่านใน `ui_private.h` |
| `test-ui-theme` | `ui_theme.c` | การอ่านและสลับไฟล์ธีม |
| `test-ui-scroll-scene` | `ui_scroll_scene.c` | การใช้ภาพเลื่อนซ้ำเทียบกับการวาดใหม่ทั้งหมด การเลื่อนรับข้อความใหม่และการค่อย ๆ ปรากฏเมื่อเปิดแชต ทั้งแบบปกติและแบบลดการเคลื่อนไหว |
| `test-ui-call-fullscreen` | `ui_call_fullscreen.c` | วิดีโอคอลเต็มจอ |
| `test-ui-lang` | `ui_lang.c` | การเลือกภาษาและตารางคำแปล รวมถึงตัวแปลงรูปแบบของ printf |

รายการ `TESTS` ใน Makefile คือชุดที่ถูกต้องที่สุด

## เทสต์แยกโมดูลออกมาอย่างไร

### 1. แทนที่ `line_rpc`

เทสต์โปรโตคอลลิงก์โมดูลที่ทดสอบกับ `thrift.o` แต่ **ไม่** ลิงก์ `transport.o` แล้วนิยาม `line_rpc()` ของตัวเอง fixture ใน [`tests/friends.c`](../../tests/friends.c) คือแบบอย่าง

```c
struct rpc_step {
	const char *path, *method, *request, *reply;
};

J *line_rpc(Line *s, const char *path, const char *method, J *args, GError **error)
{
	const struct rpc_step *step = &script[calls++];

	g_assert_cmpstr(path, ==, step->path);
	g_assert_cmpstr(method, ==, step->method);
	/* เข้ารหัสคำขอจริงด้วยตัวเข้ารหัสจริง ถอดรหัสกลับ
	 * แล้วเทียบทีละฟิลด์กับ JSON ที่คาดไว้ */
	...
	/* เข้ารหัสคำตอบตามสคริปต์เป็น result ของ Thrift แล้วถอดรหัสด้วยตัวถอดรหัสจริง
	 * ทั้งความสำเร็จและข้อผิดพลาดของบริการจึงผ่านเส้นทางจริง */
	return line_decode_reply(method, response, bytes, &s->service_code, error);
}
```

เทสต์หนึ่งตัวจึงเป็นสคริปต์ของการเรียกที่คาดไว้

```c
static const struct rpc_step steps[] = {
	{ "/RE4", "findContactBySearchIdOrTicketV3", FIND_ID, "{\"0:obj\": {…}}" },
	{ "/RE4", "addFriendByMid", "{…}", "{\"0:obj\": {}}" },
};
use(steps, G_N_ELEMENTS(steps));
```

วิธีนี้ตรวจสองอย่างพร้อมกัน คือ **คำขอ** Thrift ที่โค้ดส่งจริงทุกตัวอักษร (พาธ เมธอด หมายเลขฟิลด์ ชนิด ค่า) และวิธีที่โค้ดจัดการ **คำตอบ** ที่สมจริง รวมถึง service exception (`"{\"1:obj\": {\"1:i32\": 10}}"`) การหมดเวลา (`"timeout"`) และเมธอดที่ไม่มีให้ (`"unavailable"`)

### 2. ห่อ libcurl

เทสต์ระดับ HTTP (`test-transport`, `test-obs`, `test-shared-content`, `test-flex-asset`) ลิงก์ด้วย `-Wl,--wrap=curl_easy_setopt,--wrap=curl_easy_perform,--wrap=curl_easy_getinfo` ลิงเกอร์จะส่งการเรียกไปที่ `__wrap_curl_easy_perform()` และพวกในเทสต์ ซึ่งบันทึกตัวเลือกและเฮดเดอร์ไว้ และเล่นสถานะ เฮดเดอร์ และ body ตามสคริปต์ ดู [`tests/transport.c`](../../tests/transport.c)

### 3. ห่อ SDL และ dlopen

เทสต์การโทรและเสียงห่อ `SDL_OpenAudioDevice`, `SDL_CloseAudioDevice` และ `SDL_QueueAudio` จึงไม่ต้องมีการ์ดเสียง และเทสต์ตรวจได้ว่าอะไรจะถูกเล่น `test-qr-scan` ห่อ `dlopen` เทสต์การวาด UI และเบนช์มาร์กห่อ `SDL_RenderPresent`

### 4. มีเดียสังเคราะห์และ fixture ในเครื่อง

เทสต์มีเดียสร้างรูป เสียง และวิดีโอเอง เทสต์การโทรรัน relay UDP ในเครื่องที่พูด PLANET ได้พอให้สองฝั่งต่อกันได้ เครื่องยนต์การโทร SRTP และโคเดกตัวจริงจึงทำงานครบทั้งเส้นทางบนเครื่องของคุณ

### 5. ตัวตนสังเคราะห์

MID ทุกตัวในเทสต์เห็นได้ชัดว่าปลอม (`u00000000000000000000000000000001`) และกุญแจทุกตัวถูกสร้างขึ้นหรือเป็นเวกเตอร์ทดสอบคงที่ ห้ามใส่โทเค็น กุญแจ หรือเนื้อหาข้อความจริงในเทสต์เด็ดขาด

## การเขียนเทสต์ใหม่

1. สร้าง `tests/<name>.c`

   ```c
   // SPDX-License-Identifier: GPL-3.0-or-later
   #include "<module>.h"

   static void my_case(void)
   {
   	g_autoptr(GError) error = NULL;
   	...
   	g_assert_no_error(error);
   }

   int main(int argc, char **argv)
   {
   	g_test_init(&argc, &argv, NULL);
   	g_test_add_func("/<name>/my-case", my_case);
   	return g_test_run();
   }
   ```

2. เพิ่ม `build/test-<name>` เข้า `TESTS` ใน Makefile
3. เพิ่มกฎที่ระบุ object ของมัน และแฟล็ก wrap ถ้ามี

   ```make
   build/test-<name>: tests/<name>.c build/<module>.o build/thrift.o
   ```

   สูตรร่วม `$(TESTS):` เป็นผู้ลิงก์
4. รัน `make build/test-<name> && ./build/test-<name>` แล้วรัน `make check`
5. รัน `make format` และ `make checkpatch` ไฟล์เทสต์เก็บสตริง JSON ยาว ๆ แบ่งหลายบรรทัดได้ Makefile ผ่อนกฎของ checkpatch ข้อนี้ให้สำหรับ `tests/`

**เทสต์โปรโตคอลที่ดีควรตรวจ:** คำขอที่ตรงทุกตัวอักษร (พิมพ์หมายเลขฟิลด์ผิดจะล้มเหลว) คำตอบที่สำเร็จ ข้อผิดพลาดของบริการอย่างน้อยหนึ่งแบบ การตรวจอินพุต (ว่าง ยาวเกิน MID ผิดชนิด) และไม่มีอะไรถูกส่งเมื่อการตรวจอินพุตไม่ผ่าน

## เบนช์มาร์กและเทสต์ตัววาด

**เบนช์มาร์กการวาด** `make bench` วาด fixture ขนาด 920 × 630 ที่มีผู้ติดต่อ 900 คนและข้อความ 50 ข้อความด้วยไดรเวอร์วิดีโอ dummy ของ SDL และรายงานเวลาวาดและส่งงานฝั่ง CPU เพื่อทดสอบเส้นทางการเลื่อนบน X11 และเทียบเฟรมบางส่วนเฟรมสุดท้ายกับการวาดใหม่ทั้งหมด

```sh
xvfb-run -a env LINE_REDUCE_MOTION=1 LINE_BENCH_VIDEODRIVER=x11 LINE_BENCH_DAMAGE=1 ./build/render-bench
```

เบนช์มาร์กวาดเฟรมติดกันไปเรื่อย ๆ เมื่อใช้ `LINE_BENCH_VIDEODRIVER=wayland` กับโหมดนำเสนอที่รอจอ เช่น FIFO ธรรมดา แต่ละเฟรมจะใช้เวลาหนึ่งรอบรีเฟรช (8.3 ms ที่ 120 Hz) ผลที่ได้จึงวัดจอ ไม่ใช่ตัววาด บน NVIDIA โมดูลใช้ FIFO latest ready ตัวเลขจึงเป็นต้นทุนของตัววาดเอง เทียบสองแบบได้ด้วย `AVERNELLA_PRESENT_MODE=fifo`

**Vulkan** ต้องมี GPU และ Vulkan SDK

```sh
make vulkan-smoke                                   # ไม่แสดงบนจอ: วาดแล้วอ่านพิกเซลกลับ
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation make vulkan-smoke VK_VENDOR=0x10de
AVERNELLA_VK_DESCRIPTORS=buffer make vulkan-smoke VK_VENDOR=0x1002
make vulkan-preview VK_PREVIEW_ARGS=--test          # ตัวนำเสนอจริง: ปรับขนาด ซ่อน นำเสนอ
```

รันพร้อม validation layer ทุกครั้งที่แก้ `src/render/`

## การฟัซ

ตัวแยกวิเคราะห์ที่อ่านข้อมูลจากเครือข่ายมีฟัซเซอร์แบบนำทางด้วย coverage อยู่ใน [`fuzz/`](../../fuzz) ฟัซเซอร์เหล่านี้ไม่ได้อยู่ใน `make check`

| ฟัซเซอร์ | ครอบคลุม |
| -------- | -------- |
| `fuzz-pdtp` | `call_pdtp_receive()`: SRTP, RTP และคำเส้นทางของห้องประชุม, หมายเลขแพ็กเก็ต, ทุกองค์ประกอบ, การประกอบสตรีม, ACK, การเลือกผู้นำเสนอ |
| `fuzz-video` | `call_video_receive()`: ตัวบรรยาย EVS3 และ RFC 7741, การประกอบชิ้นส่วน, การถอดรหัส VP8 และ `call_share_decode()` |
| `fuzz-pb` | `pb_next()` และผู้ใช้ใน `call_wire.c` และ `call_schema.c` รวมถึงเฟรมสัญญาณที่เข้ารหัส |
| `fuzz-group` | คำตอบของการโทรกลุ่ม, presence, คำขอสตรีมและกล้อง, RTCP feedback ใน `call_group.c` |
| `fuzz-thrift` | ตัวอ่าน Compact และ Binary ใน `thrift.c` |

```sh
make fuzz                       # บิลด์ลง build-fuzz/ และเขียนอินพุตตั้งต้น
make fuzz-run FUZZ_TIME=600     # รันทุกฟัซเซอร์พร้อมกันตัวละ 10 นาที
make fuzz-run-pdtp              # ฟัซเซอร์ตัวเดียว
make fuzz-check                 # เล่นอินพุตตั้งต้นและ corpus ที่เก็บไว้ซ้ำหนึ่งรอบ
./build-fuzz/fuzz-pdtp build-fuzz/artifacts/pdtp-crash-…   # ทำให้ความล้มเหลวเกิดซ้ำ
```

GCC ไม่มี libFuzzer ดังนั้น [`fuzz/driver.c`](../../fuzz/driver.c) จึงเป็นเอนจิน `-fsanitize-coverage=trace-pc,trace-cmp` ของ GCC ส่ง edge coverage และค่าทั้งสองฝั่งของการเปรียบเทียบที่ไม่ผ่านให้เอนจิน ทุกอย่างบิลด์ด้วย AddressSanitizer และ UndefinedBehaviorSanitizer ใน `build-fuzz/` แยกจาก `build/` ฮาร์เนสใช้ `LLVMFuzzerTestOneInput()` ของ libFuzzer และไดรเวอร์รับแฟล็กหลักของ libFuzzer (`-max_total_time`, `-runs`, `-max_len`, `-timeout`, `-artifact_prefix`) ส่งแฟล็กเพิ่มผ่าน `FUZZ_ARGS`

คอมเมนต์ของฟัซเซอร์แต่ละตัวอธิบายรูปแบบอินพุต ฟัซเซอร์ PDTP และวิดีโอถือคีย์ SRTP สังเคราะห์ชุดเดียวกับ `tests/pdtp.c` และ `tests/call_video.c` จึงเข้ารหัสข้อความที่ฟัซก่อนส่งถึงตัวแยกวิเคราะห์ [`fuzz/seeds.c`](../../fuzz/seeds.c) เขียนอินพุตตั้งต้น โดยสร้าง fixture ของเทสต์ขึ้นใหม่ และบันทึกสิ่งที่ตัวเข้ารหัสของ Avernella ส่งออกเมื่อใช้คีย์และภาพสังเคราะห์ชุดเดียวกัน

อินพุตใหม่สะสมใน `build-fuzz/corpus/NAME/` และ log อยู่ใน `build-fuzz/NAME.log` อินพุตที่ทำให้แครช หน่วยความจำรั่ว หมดเวลา หรือหน่วยความจำไม่พอ จะถูกบันทึกเป็น `build-fuzz/artifacts/NAME-crash-HASH` (`leak-`, `timeout-`, `oom-`) แก้ด้วยเทสต์ถดถอยในไฟล์ `tests/` ที่ตรงกัน

## สิ่งที่เทสต์ครอบคลุมไม่ได้

การทำงานร่วมกับ LINE จริง: การลงชื่อเข้าใช้จริง การส่งถึงบัญชีอื่น มีเดียบนเซิร์ฟเวอร์ของ LINE การโทรกับแอป LINE บนโทรศัพท์และเดสก์ท็อป สิ่งเหล่านี้ต้องใช้บัญชีจริงและอุปกรณ์เครื่องที่สอง [analysis/desktop-features.md](../../analysis/desktop-features.md) และ [analysis/video-call-wire.md](../../analysis/video-call-wire.md) บันทึกว่าอะไรยืนยันกับของจริงแล้ว เมื่อคุณยืนยันอะไรกับของจริง ให้บันทึกไว้ที่นั่นโดยไม่มีข้อมูลส่วนตัว
