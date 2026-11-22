# การมีส่วนร่วมพัฒนา

[สารบัญ](README.md) · ก่อนหน้า: [การทดสอบ](testing.md)

หน้านี้รวบรวมธรรมเนียมที่โค้ดใช้ สูตรทีละขั้นสำหรับการเปลี่ยนแปลงที่พบบ่อย ข้อผิดพลาดที่ทำกันง่าย และวิธีที่แกะโปรโตคอลมา

## สไตล์โค้ด

ซอร์ส C และเทสต์ทำตาม [สไตล์โค้ดของเคอร์เนล Linux](https://docs.kernel.org/process/coding-style.html): ย่อหน้าด้วยแท็บ 80 คอลัมน์ วงเล็บปีกกาแบบ K&R คอมเมนต์แบบ `/* */` และชื่อตัวพิมพ์เล็กคั่นด้วยขีดล่าง

```sh
make format         # ใช้ .clang-format กับซอร์สทุกไฟล์
make format-check   # ล้มเหลวถ้ามีไฟล์ที่ยังไม่ได้จัดรูปแบบ
make checkpatch     # checkpatch.pl ของเคอร์เนล ทีละไฟล์
```

- ทดสอบ `clang-format` กับเวอร์ชัน 23 ตารางยาว ๆ ถูกกันไว้ด้วย `/* clang-format off */` … `/* clang-format on */` ตามจำเป็น
- `checkpatch.pl` มาจาก header ของเคอร์เนลที่ใช้อยู่ (`linux-headers` บน Arch, `linux-cachyos-headers` หรือแพ็กเกจของเคอร์เนลรุ่นที่ใช้บน CachyOS) หลังอัปเกรดเคอร์เนล ให้รีบูตหรือตั้ง `CHECKPATCH=/usr/lib/modules/<version>/build/scripts/checkpatch.pl`
- `.checkpatch-typedefs` ระบุชนิดของ GLib, SDL และไลบรารีอื่นที่ checkpatch ไม่รู้จัก
- Makefile เพิกเฉยกฎเฉพาะของเคอร์เนลที่ไม่เหมาะกับโปรแกรม GLib (`NEW_TYPEDEFS`, `AVOID_EXTERNS`, `EMBEDDED_FUNCTION_NAME`, `FILE_PATH_CHANGES`, `REPEATED_WORD`, `MACRO_WITH_FLOW_CONTROL`) และสำหรับเทสต์ยังเพิกเฉย `SPLIT_STRING` กับ `MISSING_SPACE` ด้วย
- ทุกไฟล์ขึ้นต้นด้วยบรรทัด SPDX: `// SPDX-License-Identifier: GPL-3.0-or-later` ในไฟล์ `.c` และ `/* SPDX-License-Identifier: GPL-3.0-or-later */` ใน header
- โค้ด build ได้สะอาดด้วย `-Wall -Wextra -Wpedantic -Wformat=2` ให้รักษาไว้แบบนั้น

## ธรรมเนียม

### หน่วยความจำและความเป็นเจ้าของ

- ใช้การเก็บกวาดอัตโนมัติของ GLib: `g_autoptr(J)`, `g_autoptr(GError)`, `g_autofree char *` `line.h` นิยาม `G_DEFINE_AUTOPTR_CLEANUP_FUNC(J, json_object_put)` ไว้แล้ว
- คอมเมนต์ของฟังก์ชันบอกว่าใครเป็นเจ้าของผลลัพธ์ "Caller owns" แปลว่าคุณต้องปล่อยเอง "Borrowed" แปลว่าห้ามปล่อย
- json-c: `json_object_object_add()` **รับ** ความเป็นเจ้าของของค่าไป ถ้าจะเพิ่มอ็อบเจกต์ที่คุณยังเก็บไว้ ให้ส่ง `json_object_get(value)` `field()` และ `strfield()` คืนพอยน์เตอร์แบบยืม
- ล้างความลับเมื่อใช้เสร็จ: ใช้ `OPENSSL_cleanse()` หรือ `explicit_bzero()` กับกุญแจ รหัสผ่าน โทเค็น และ payload ที่ถอดรหัสแล้ว ดูวิธีที่ `sealing.c` และ `password_login.c` ทำ

### ข้อผิดพลาด

- ฟังก์ชันที่ล้มเหลวได้รับ `GError **error` เป็นอาร์กิวเมนต์สุดท้าย และคืน `NULL` หรือ `FALSE` เมื่อล้มเหลว
- ใช้โดเมน `LINE_ERROR` และรหัสใน `line.h` เมื่อผู้เรียกต้องแยกแยะความล้มเหลว รหัส 1 คือข้อผิดพลาดทั่วไป
- **ข้อความผิดพลาดแสดงต่อผู้ใช้ตรงตามที่เขียน** เขียนเป็นประโยคสั้น ๆ ภาษาง่าย ๆ ที่บอกว่าเกิดอะไรขึ้น และถ้าเป็นไปได้ให้บอกว่าต้องทำอะไร เช่น "Select a conversation from your chat list." หรือ "The QR code expired. Choose sign in to request a new one."
- หลังเรียกบริการที่มีชุดรหัสข้อผิดพลาดของตัวเอง (RelationService, ShopService, การต่ออายุโทเค็น) ให้ตั้ง `s->service_code = -1` ผู้เรียกที่อ่านรหัสของ TalkService จะได้ไม่เข้าใจผิด

### กฎของเธรด

- **UI:** มีแค่เธรดหลักที่แตะ `ui` และสถานะของโมดูล เธรดอื่นส่ง SDL user event
- **Worker:** มีแค่เธรดหลักที่แตะ `Line` (เซสชัน) เธรด sync รูป การโทร และ screencast ได้สำเนาของสิ่งที่ต้องใช้
- ห้ามให้ access token ของบัญชีแก่เธรดการโทร เธรดรูป หรือ UI

### อินพุตจาก LINE เชื่อถือไม่ได้

ตรวจทุกค่าก่อนใช้: ชนิด (`strfield()` คืน `NULL` ถ้าค่าไม่ใช่สตริง) ความยาว ชุดอักขระ (MID, ID, โทเค็น) ช่วงของค่า และ URL ดูสไตล์ได้จาก `talk_id()`, `valid_mid()`, `line_emoji_id_valid()` และ `route_token()` ปฏิเสธดีกว่าพยายามซ่อม

### อินพุตจาก UI ก็เชื่อถือไม่ได้

worker ถือว่า UI น่าเชื่อถือน้อยกว่าตัวเอง มันตรวจพารามิเตอร์ซ้ำ ค้นข้อความจากสำเนาของตัวเอง (`w->messages`) แทนการรับเนื้อหาข้อความจาก UI และอนุญาตการกระทำเฉพาะกับแชต เพื่อน ผลการค้นหา และการ์ดที่ตัวเองเคยแสดง

### ข้อความที่ผู้ใช้เห็น

ห่อข้อความในส่วนติดต่อด้วย `_()` (หรือ `N_()` ในตารางแบบ static) เพื่อให้แปลได้ผ่าน `src/ui_lang.c` ข้อความผิดพลาดของ worker เดินทางไปถึง UI เป็นข้อความ

## สูตร: เรียก RPC ใหม่ของ LINE

สมมติ LINE มี `TalkService.getSomething(1: GetSomethingRequest request)` และคุณต้องการแผง "Something"

1. **หานิยาม** หาหมายเลขฟิลด์และชนิดของอาร์กิวเมนต์และผลลัพธ์จากโค้ดของ LINE (ดูหัวข้อวิธีแกะโปรโตคอลด้านล่าง) หรือแหล่งอ้างอิงอย่าง `line.thrift` ของ LINEJS ถ้าเป็นของใหม่ให้จดไว้ใน `analysis/`
2. **เพิ่มฟังก์ชันโปรโตคอล** ในโมดูลที่เหมาะสม เช่น `messaging.c`

   ```c
   J *line_something(Line *s, const char *chat, GError **error)
   {
   	if (!talk_id(chat)) {
   		g_set_error_literal(error, LINE_ERROR, 1, "Select a conversation.");
   		return NULL;
   	}
   	g_autoptr(J) args = obj();
   	J *req = obj();

   	json_object_object_add(args, "1:obj", req);
   	json_object_object_add(req, "1:str", json_object_new_string(chat));
   	g_autoptr(J) reply = line_rpc(s, "/S4", "getSomething", args, error);
   	if (!reply)
   		return NULL;
   	/* ตรวจแล้วแปลงเป็นแถวเล็ก ๆ ให้ UI */
   	J *row = obj();
   	const char *name = strfield(reply, "2");

   	json_object_object_add(row, "name", json_object_new_string(name ? name : ""));
   	return row;
   }
   ```

   ประกาศไว้ใน header ของโมดูล พร้อมคอมเมนต์บอกความเป็นเจ้าของ
3. **ถ้ามันเปลี่ยนสถานะ** ต้องใช้ลำดับคำขอ: จองใน `dispatch()` ด้วย `reserve_sequence()` แล้วส่งเข้าไป (แบบที่ `groups.c` ทำ) หรือให้ฟังก์ชันทำ `++s->seq` หลังจาก `dispatch()` จองแล้ว (แบบที่ `messaging.c` ทำ) ห้ามส่ง RPC ที่เปลี่ยนสถานะโดยไม่ได้จองลำดับไว้
4. **เปิดให้ UI ใช้** ด้วยกิ่งใหม่ใน `dispatch()` ของ `main.c` และตรวจพารามิเตอร์ที่นั่นด้วย
5. **เขียนเทสต์** ด้วย fixture ของ `line_rpc` (ดู [การทดสอบ](testing.md)): คำขอที่ตรงทุกตัวอักษร คำตอบที่ดี ข้อผิดพลาดของบริการ อินพุตที่ไม่ถูกต้อง
6. **เรียกจาก UI** ด้วย `ui_rpc("something", params, tag)` แล้วจัดการคำตอบในฟังก์ชัน `_reply()` ของโมดูลคุณ
7. **อัปเดตเอกสาร**: ตารางเมธอดใน [พื้นฐานโปรโตคอล](protocol-basics.md) และ [สถาปัตยกรรม](architecture.md) และหน้าของหมวดนั้น ทั้งภาษาอังกฤษและภาษาไทย

## สูตร: จัดการ operation ชนิดใหม่

1. หาหมายเลข OpType และความหมายของ param
2. ถ้ามันแก้ UI ตรงจุดได้ ให้เพิ่ม case ใน `social_operation()` (หรือฟังก์ชันใหม่ที่ `live_operations()` เรียก) ใน `main.c` ซึ่งส่งอีเวนต์เล็ก ๆ ถ้าไม่ใช่ ก็ไม่ต้องทำอะไร operation ที่ไม่ได้จัดการจะทำให้รีเฟรชทั้งหมดอยู่แล้ว
3. จัดการอีเวนต์ใน `handle_packet()` ของ `ui.c` หรือในโมดูลที่เกี่ยวข้อง
4. เพิ่มเคสทดสอบใน `tests/live_sync.c` หรือ `tests/talk_sync.c` ถ้าเกี่ยวกับตรรกะการแบ่งหน้า
5. เพิ่มลงตารางใน [Sync](sync.md)

## สูตร: เพิ่มฟีเจอร์ UI

ดูหัวข้อการเพิ่มหน้าจอหรือ overlay ใหม่ใน [ส่วนติดต่อผู้ใช้](ui.md)

## ข้อผิดพลาดที่พบบ่อย

- **ใช้ลำดับคำขอซ้ำ** RPC ที่เปลี่ยนสถานะใดก็ตามที่ไม่มี `reserve_sequence()` (หรือมี `++s->seq` เกินมา) อาจทำให้ LINE ทิ้งหรือทำซ้ำการกระทำ อ่านเรื่องลำดับคำขอใน [การลงชื่อเข้าใช้](sign-in.md)
- **ใส่ความลับใน metadata** `contentMetadata` ไม่ได้เข้ารหัส ข้อมูลลับต้องอยู่ใน payload ที่ปิดผนึก
- **ใช้ offset แบบไบต์แทน UTF-16** ช่วงของอีโมจิและเมนชันเป็นหน่วยรหัส UTF-16 ภาษาไทยและอีโมจิทำให้เห็นความต่างชัด
- **ลืมว่า `json_object_object_add()` รับความเป็นเจ้าของ** การเพิ่มอ็อบเจกต์ที่ยืมมาโดยไม่ `json_object_get()` จะทำให้ปล่อยหน่วยความจำซ้ำในภายหลัง
- **ถือว่าทุก service code เป็น TalkException** ตั้ง `s->service_code` กลับหลังเรียกบริการอื่น
- **บล็อกเธรด UI** ห้ามทำ I/O กับไฟล์หรือเครือข่ายบนเธรดหลักของ UI ให้ขอผ่าน worker หรือใช้เธรดช่วยที่ส่ง SDL event กลับมา
- **ทำให้การใช้ภาพเลื่อนซ้ำพัง** ถ้าเนื้อหาใหม่ของคุณเปลี่ยนเองได้ (แอนิเมชัน การโหลดที่มาช้า) ต้องให้ `reusable_history()` คืนเท็จขณะมันมองเห็นอยู่ ไม่อย่างนั้นการเลื่อนจะแสดงพิกเซลเก่า
- **ถือว่ามีโมดูล Vulkan เสมอ** ทดสอบกับ `LINE_DISABLE_GPU=1` ด้วย
- **ทดสอบแค่ธีมเดียวหรือความกว้างเดียว** ตรวจทั้งธีม LINE และ Avernella หน้าต่างแบบกะทัดรัด (≤ 640 px) และโหมดลดการเคลื่อนไหว
- **โทเค็นรั่วลง log** ข้อความดีบักห้ามพิมพ์โทเค็น กุญแจ ที่อยู่หน้า LIFF หรือเนื้อหาข้อความ
- **รันสองตัวพร้อมกัน** ล็อกของเซสชันจะปฏิเสธ ให้ปิดหน้าต่างแรกก่อน รวมถึงจาก tray
- **สลับ `DEBUG=1` โดยไม่ `make clean`** ไฟล์ object ไม่ถูก build ใหม่เมื่อเปลี่ยนแค่แฟล็ก
- **แก้ไฟล์ใน `build/`** ไฟล์พวกนั้นสร้างอัตโนมัติ ให้แก้ซอร์สและ Makefile แทน

## วิธีที่แกะโปรโตคอลมา

โปรโตคอลมาจาก LINE สำหรับ Android 26.16.0 (`jp.naver.line.android`) ไฟล์ `.apkm` เก็บไว้ที่รากของรีโพและ git ละไว้ ห้ามคอมมิตเด็ดขาด

1. **แตกโค้ด** unzip `base.apk` ออกจาก bundle แล้ว unzip ไฟล์ `classes*.dex` ออกจาก `base.apk` split ของ ABI มีไลบรารีเนทีฟ เช่น `libandromeda.so` สำหรับการโทร
2. **อ่าน struct ของ Thrift** โค้ดของ LINE ถูกทำให้อ่านยาก (คลาสชื่ออย่าง `lf0/n1`) แต่คลาสที่ Thrift สร้างยังเก็บตารางฟิลด์ไว้ static initializer (`<clinit>`) ของแต่ละ struct สร้างอ็อบเจกต์ `TField(name, type, id)` ด้วยอาร์กิวเมนต์คงที่ และของ enum สร้าง `(NAME, ordinal, value)` สคริปต์เล็ก ๆ ที่ไล่คำสั่ง DEX `const-string`, `const` และ `invoke-direct <init>` ในแต่ละ `<clinit>` จะกู้ชื่อ ชนิด และหมายเลขฟิลด์ของทุก struct และค่าของทุก enum ได้
3. **อ่าน service client** service client อ้างถึงชื่อเมธอดเป็นสตริง (`"issueLiffView"`) ข้าง ๆ คลาส `_args` และ `_result` ที่มันสร้าง ฟิลด์ของคลาส result บอกชนิดที่สำเร็จและ exception
4. **อ่านโมเดล JSON** descriptor ของ Kotlin serialization ระบุคีย์ JSON ของโมเดลที่ไม่ใช่ Thrift เช่น markup ของ rich message
5. **ไล่ตามพฤติกรรม** ตัวแปลงย้อน Dalvik เล็ก ๆ ที่พิมพ์บรรทัด `const-string`, `new-instance` การเข้าถึงฟิลด์ และ `invoke` ก็พอสำหรับไล่ว่าค่าไหลไปทางไหน เช่น ฟิลด์ไหนของคำตอบกลายเป็นพารามิเตอร์ใน URL
6. **โค้ดเนทีฟ** การส่งสัญญาณการโทรและรูปแบบมีเดียอ่านจาก `libandromeda.so`: descriptor ของ protobuf-c เก็บชื่อและหมายเลขฟิลด์ไว้ และฟังก์ชันเฉพาะถูกแปลงย้อนหรือรันใต้การจำลอง CPU ด้วยอินพุตสังเคราะห์
7. **ตรวจทานกับแหล่งสาธารณะ** LINE เผยแพร่โค้ดฝั่งไคลเอนต์บางส่วน เช่น LIFF SDK ที่ `https://static.line-scdn.net/liff/edge/2/sdk.js` โปรเจกต์โอเพนซอร์สที่ทำงานร่วมกับ LINE ([line-rs](https://github.com/softnetics/line-rs/tree/addf69c2e886a2364a396ce360093a62119dcb32), [LINEJS](https://github.com/evex-dev/linejs/tree/v3.6.5), bridge LINE ของ Beeper) ถูกใช้ยืนยันรูปแบบ ประกาศสัญญาอนุญาต MIT ของโปรเจกต์เหล่านี้อยู่ใน `analysis/`

จดสิ่งที่ค้นพบไว้ใน `analysis/` พร้อมชื่อคลาส หมายเลขฟิลด์ ที่อยู่ และหลักฐาน แบบเดียวกับบันทึกที่มีอยู่ เก็บข้อมูลส่วนตัว (บัญชี เบอร์โทร เนื้อหาข้อความ โทเค็น) ไว้นอกบันทึก เทสต์ และคอมมิต

## ก่อนส่งการเปลี่ยนแปลง

- [ ] `make` build ได้โดยไม่มีคำเตือน
- [ ] `make check` ผ่าน
- [ ] `make format-check` และ `make checkpatch` ผ่าน
- [ ] โค้ดโปรโตคอลใหม่มีเทสต์แบบ fixture
- [ ] UI ใหม่ใช้ได้ทั้งสองธีม ที่ความกว้างแบบกะทัดรัด และกับ `LINE_DISABLE_GPU=1`
- [ ] อัปเดตเอกสาร **ทั้ง** [`docs/en/`](../en/README.md) และ [`docs/th/`](README.md)

## สัญญาอนุญาต

Avernella เผยแพร่ภายใต้ GNU General Public License เวอร์ชัน 3 หรือ (ตามที่คุณเลือก) เวอร์ชันใดก็ได้ที่ใหม่กว่า การจำลองชิปเสียง NES ใน `chiptune.c` พอร์ตมาจาก Mesen ภายใต้สัญญาอนุญาตเดียวกัน การมีส่วนร่วมพัฒนาถูกรับภายใต้เงื่อนไขเดียวกัน ดู [README](../../README.md)
