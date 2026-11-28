# มีเดีย สติกเกอร์ อีโมจิ และเนื้อหาพิเศษ

[สารบัญ](README.md) · ก่อนหน้า: [Letter Sealing](letter-sealing.md) · ถัดไป: [เพื่อน โปรไฟล์ และกลุ่ม](social.md)

หน้านี้ครอบคลุมทุกอย่างที่ไม่ใช่ข้อความธรรมดา: ไฟล์แนบที่เข้ารหัส รูปโปรไฟล์ สติกเกอร์ อีโมจิ LINE, Flex และ rich message จากบัญชีทางการ และอัลบั้มกับโน้ต

## ไฟล์แนบที่เข้ารหัส

รูป วิดีโอ เสียง และไฟล์ อยู่บน **object storage** (OBS) ของ LINE ที่ `obs.line-apps.com` ตัวข้อความบรรจุแค่การอ้างอิงถึง object ที่เก็บไว้ และกุญแจสำหรับถอดรหัสซึ่งปิดผนึกด้วย Letter Sealing โค้ดอยู่ใน [`media.c`](../../src/media.c) (ลำดับขั้นตอน) [`media_crypto.c`](../../src/media_crypto.c) (การเข้ารหัส) และ [`obs_http.c`](../../src/obs_http.c) (การเรียก HTTP)

### ชนิด

| ชนิด | ชนิดเนื้อหา | namespace ของ OBS (`SID`) | ชื่อที่อัปโหลด |
| ---- | ----------- | ------------------------- | -------------- |
| รูป | 1 | `emi` | `<uuid>.jpg`, `.png`, `.gif` หรือ `.webp` |
| วิดีโอ | 2 | `emv` | `<uuid>.mp4` |
| เสียง | 3 | `ema` | `<uuid>.m4a` |
| ไฟล์ | 14 | `emf` | `<uuid>.bin` |

ตัว `e` ย่อมาจาก "encrypted" มีเดียเก่าที่ไม่เข้ารหัสอยู่ใน namespace `m` โดยใช้รหัสข้อความเป็นชื่อ

ขีดจำกัด: 64 MiB ต่อไฟล์แนบ (`LINE_MEDIA_LIMIT`) และ 2 MiB สำหรับภาพตัวอย่าง รูปต้องเป็น JPEG, PNG, GIF หรือ WebP จริง ๆ `image_format()` ตรวจจาก magic byte

### การเข้ารหัส

`line_media_encrypt()` และ `line_media_decrypt()`

1. สร้าง **key material** สุ่ม 32 ไบต์ต่อหนึ่งไฟล์แนบ
2. **HKDF-SHA256** แบบไม่มี salt และใช้ info `"FileEncryption"` ขยายเป็น 76 ไบต์: กุญแจ AES 32 ไบต์ กุญแจ HMAC 32 ไบต์ และ nonce 12 ไบต์ บล็อกตัวนับของ AES-CTR ขนาด 16 ไบต์คือ nonce ตามด้วยไบต์ศูนย์สี่ไบต์
3. ไฟล์ถูกเข้ารหัสด้วย **AES-256-CTR**
4. ต่อท้ายด้วยแท็ก **HMAC-SHA256** 32 ไบต์
   - สำหรับรูป เสียง และไฟล์ คำนวณจาก ciphertext ทั้งก้อน
   - สำหรับวิดีโอ คำนวณจาก **digest SHA-256 ของ ciphertext ทีละ 128 KiB** ที่นำมาต่อกัน รายการ digest ถูกอัปโหลดด้วย ตัวเล่นจึงตรวจวิดีโอได้ทีละส่วน

สำหรับวิดีโอต้นฉบับ เครื่องผู้รับตรวจแท็กจากรายการ digest ของแต่ละส่วน ส่วนรูป ไฟล์ เสียง และภาพตัวอย่างวิดีโอจะตรวจแท็กจาก ciphertext โดยตรง ชนิดข้อความที่ใช้เลือกวิธีตรวจได้รับการยืนยันด้วย Letter Sealing v2 ส่วน v1 รองรับเฉพาะข้อความตัวอักษร เครื่องผู้รับเปรียบเทียบแท็กแบบเวลาคงที่ก่อนถอดรหัส และตรวจขนาดของไฟล์ต้นฉบับเมื่อข้อความระบุ `FILE_SIZE`

ภาพตัวอย่างใช้กุญแจ MAC เดียวกับวิดีโอต้นฉบับ ถ้าขนาด ciphertext ของภาพตัวอย่างหารด้วย 32 ลงตัว จะมีขนาดตรงกับรายการ digest แบบ SHA-256 ได้ Avernella จึงข้ามภาพตัวอย่างนั้น โดยยังเปิดวิดีโอต้นฉบับได้

### การส่งไฟล์แนบ

`send_attachment_bytes()` ทำตามขั้นตอนนี้ตามลำดับ

1. **ตรวจสอบ** แชต (ผู้ใช้หรือกลุ่ม) และชนิด แล้วอ่านไฟล์ (`read_attachment()`)
2. **สร้าง key material** และ payload ที่จะปิดผนึก `{"keyMaterial": "<base64>", "fileName": "<ชื่อ>"}` ใส่ชื่อไฟล์เฉพาะชนิดไฟล์
3. **ปิดผนึก payload ก่อน** ด้วย `line_seal_payload(chat, ชนิดเนื้อหา, payload)` ซึ่งต่อรองกุญแจของผู้รับ *ก่อน* ส่งไบต์ใด ๆ ไปที่ OBS ถ้าผู้ติดต่อรับ Letter Sealing v2 ไม่ได้ จะไม่มีอะไรถูกอัปโหลด
4. **เข้ารหัส** ไฟล์ วิดีโอจะได้รายการ digest ทีละส่วนด้วย
5. **ขอโทเค็น OBS**: `acquireEncryptedAccessToken` บน `/S4` ด้วย `{2: 2}` คำตอบเป็นสตริง `"<อายุเป็นวินาที>\x1e<token>\x1e…"` `line_obs_token()` แคชไว้ใน `s->obs_token` จนหมดอายุ และไม่เคยบันทึกลงดิสก์
6. **อัปโหลด** ciphertext: `POST https://obs.line-apps.com/r/talk/<sid>/reqid-<uuid>` พร้อมเฮดเดอร์
   - `x-line-access: <โทเค็น OBS>`
   - `x-line-application: DESKTOPWIN…`
   - `x-obs-params: <base64 ของ {"type":"FILE","ver":"2.0","name":"<ชื่อที่อัปโหลด>","range":"bytes 0-<n-1>/<n>"}>`
   - `Content-Type: application/octet-stream`

   เฮดเดอร์ `x-obs-oid` ในคำตอบให้รหัส object จริง (`OID`)
7. **อัปโหลด object ข้างเคียง** ถ้ามี ที่ `<OID>__ud-preview` (รูป) หรือ `<OID>__ud-hash` (วิดีโอ คือรายการ digest) สำหรับรูป **ภาพตัวอย่างคือ ciphertext เดียวกับต้นฉบับ** เพราะการเข้ารหัสภาพย่อแยกต่างหากด้วย key stream ของ CTR ชุดเดียวกันจะทำให้ข้อมูลรั่ว ไคลเอนต์จึงใช้ต้นฉบับซ้ำ
8. **ส่งข้อความ** ด้วย `line_send_sealed()` ชนิดเนื้อหา 1, 2, 3 หรือ 14 พร้อม chunk ห้าตัวจากขั้นที่ 3 และ metadata

   | คีย์ | ค่า |
   | ---- | --- |
   | `SID`, `OID` | ตำแหน่งของ object |
   | `FILE_SIZE` | ขนาด plaintext เป็นไบต์ |
   | `DURATION` | ความยาววิดีโอเป็นมิลลิวินาที อ่านจากเฮดเดอร์ MP4 (`video_duration()`) |
   | `MEDIA_CONTENT_INFO` | รูป: JSON `{"category":"original","fileSize":…,"extension":"…","width":…,"height":…,"animated":…}` |
   | `e2eeVersion` | `"2"` |

การอัปโหลดไม่ได้ให้สิทธิ์ใครเข้าถึง ผู้รับจะหาและถอดรหัส object ได้ก็ต่อเมื่อ `sendMessage` ส่งตำแหน่งและกุญแจไปถึงแล้ว

การ์ดไฟล์แสดงชื่อและขนาดไฟล์ตามที่ LINE ระบุ เลือก **บันทึกไฟล์แนบ** เพื่อเลือกตำแหน่งบันทึก ในหน้าตัวอย่างเลือก **ส่งเป็นไฟล์** เพื่อส่งรูปหรือคลิปในรูปแบบเดิมเป็นไฟล์แนบทั่วไป เอกสารและไฟล์บีบอัดจะใช้วิธีนี้โดยอัตโนมัติ

### การดาวน์โหลด

`line_download_attachment()` (และ `line_download_image()`, `line_download_video_preview()`)

1. เปิด payload ที่ปิดผนึกด้วย `line_open_payload()` เพื่อได้ `keyMaterial` (base64 ยาว 44 ตัวพอดี) และ `fileName` สำหรับไฟล์
2. ตรวจว่า `SID` ใน metadata ตรงกับชนิดเนื้อหา
3. `GET https://obs.line-apps.com/r/talk/<sid>/<oid>` พร้อมโทเค็น OBS และเฮดเดอร์ `x-talk-meta` ซึ่งเป็น base64 ของ `{"message": "<base64 ของ struct Message แบบ Thrift Binary {4: รหัสข้อความ, 27: รายการว่าง}>"}` (`download_header()`) บอก OBS ว่าคำขอนี้เป็นของข้อความไหน
4. ตรวจขนาดและแท็กก่อนถอดรหัส (`line_media_decrypt()`)

ภาพตัวอย่างวิดีโอมาจาก `<sid>/<oid>/ud-preview` (ถอยไปใช้ `<oid>__ud-preview`) หมดเวลาใน 6 วินาที มีเดียเก่าที่ไม่เข้ารหัสใช้ `m/<รหัสข้อความ>` และ `/preview`

ข้อความรูปภาพจากบัญชีทางการมักไม่ได้เก็บไว้ใน OBS เลย metadata ของข้อความมี `DOWNLOAD_URL` (ตัวรูป) และ `PREVIEW_URL` (รูปขนาดเล็ก) และการขอ `m/<รหัสข้อความ>` จาก OBS จะได้ 404 แอป LINE บนโทรศัพท์โหลดภาพย่อจาก `PREVIEW_URL` และตัวรูปจาก `DOWNLOAD_URL` เมื่อมีค่า `line_hosted_image_url()` เลือก URL แรกในสองตัวนี้ที่ผ่าน[นโยบายโฮสต์ใดก็ได้](#รูปจากโฮสต์ใดก็ได้) แล้ว worker ดึงด้วย `line_flex_image_download()` ทั้งสำหรับภาพย่อและการ **บันทึก**

ถ้า OBS ปฏิเสธโทเค็น (`LINE_OBS_UNAUTHORIZED`) worker จะลืมโทเค็นแล้วลองอีกครั้งเดียวด้วยโทเค็นใหม่ แบบเดียวกับไคลเอนต์ OBS ของ LINE เอง 429 หรือ 503 จะเริ่มช่วงพักของโฮสต์นั้น และคำขอไปยังโฮสต์นั้นจะล้มเหลวในเครื่องจนกว่าช่วงพักจะหมด (ดู [พื้นฐานโปรโตคอล](protocol-basics.md))

### การส่งข้อมูลไปถึงหน้าต่าง

ข้อมูลที่ดาวน์โหลดไปถึง UI เป็น media lease (ดู [สถาปัตยกรรม](architecture.md)) คือ memfd ที่ปิดผนึกซึ่ง UI เปิดผ่าน `/proc/<worker>/fd/<n>` เมธอดของ worker

| เมธอด | คืนค่า |
| ----- | ------ |
| `photo` | รูปเต็ม (หรือรูปสติกเกอร์สำหรับข้อความสติกเกอร์) |
| `videoPoster` | ภาพตัวอย่างของวิดีโอ |
| `openVideo` | วิดีโอทั้งไฟล์ สำหรับ `ffplay` |
| `saveFile` | เขียนไฟล์แนบลงพาธที่ผู้ใช้เลือกแบบ atomic (`line_save_media()`) |

### ตัวดูรูปแบบเต็มหน้าต่าง

คลิกรูปเพื่อเปิดดูแบบเต็มหน้าต่าง ใช้ล้อเมาส์เพื่อซูมโดยยึดตำแหน่งตัวชี้ จากนั้นลากรูปเพื่อดูส่วนที่ขยาย แถบเครื่องมือแบบลอยมีปุ่มซูม ปรับให้พอดีหน้าต่างหรือขนาดจริง หมุนทีละ 90 องศา และบันทึกไฟล์ต้นฉบับ ใช้ปุ่ม **ซ้าย** และ **ขวา** หรือลูกศรด้านข้างเพื่อเลื่อนไปยังรูปอื่นในหน้าประวัติที่เปิดอยู่ กด **Esc** หรือคลิกฉากหลังเพื่อปิด เมื่อเปิดลดการเคลื่อนไหว การซูมจะเปลี่ยนขนาดทันที การคำนวณตำแหน่งตัวชี้และขอบภาพดัดแปลงจากตัวดูสื่อ GPL ของ Telegram Desktop [ซอร์สโค้ดต้นทาง](https://github.com/telegramdesktop/tdesktop/blob/fca0d08b6852df2195e11db91f7a4e3be2cbc4a4/Telegram/SourceFiles/media/view/media_view_overlay_widget.cpp#L7871)

### การส่งต่อ

`line_forward_attachment()` ดาวน์โหลดและถอดรหัสต้นฉบับ แล้วส่งเป็นไฟล์แนบใหม่ด้วย **กุญแจใหม่** สำหรับแชตปลายทาง กุญแจไม่เคยถูกใช้ซ้ำข้ามแชต

## รูปโปรไฟล์

อวตารเป็นสาธารณะ โปรไฟล์ของผู้ติดต่อมีพาธรูป เช่น `/0h…` `line_avatar_download()` ดึง `https://profile.line-scdn.net<path>` แบบไม่ระบุตัวตน (ไม่เกิน 2 MiB) บนเธรดรูปของ worker และ UI ขอด้วย `avatar`

การเปลี่ยนรูปของตัวเอง (`updateMyPicture`, [`profile_image.c`](../../src/profile_image.c))

1. `line_profile_jpeg()` ถอดรหัส JPEG, PNG หรือ WebP (ไม่เกิน 20 MiB) ไฟล์ JPEG จะถูกย่อในตัวถอดรหัสของ libjpeg-turbo ก่อนเพื่อความเร็ว
2. ครอปสี่เหลี่ยมจัตุรัสตรงกลาง แล้วย่อเป็น 640 × 640 ด้วยการเฉลี่ยพื้นที่ พิกเซลโปร่งใสกลายเป็นสีขาว
3. เข้ารหัสเป็น JPEG แบบ baseline
4. `line_obs_upload_profile()` ส่งไปที่ `https://obs.line-apps.com/r/talk/p/<MID ของคุณ>` พร้อม `x-obs-params` เป็น `{"name":"profile.jpg","quality":"100","type":"image","ver":"2.0"}` OBS อัปเดตโปรไฟล์เอง ไม่ต้องเรียก RPC แยก

## สติกเกอร์

[`stickers.c`](../../src/stickers.c) สติกเกอร์เป็นสินค้าในร้าน ส่งได้เฉพาะสติกเกอร์ที่บัญชีเป็นเจ้าของ

**แค็ตตาล็อก** (`stickerCatalog`, `line_sticker_catalog()`): `getOwnedProductSummaries` บน `/TSHOP4` แบ่งหน้าด้วย offset พร้อมชนิดร้านและ locale ที่สร้างจากภาษาและภูมิภาคของคุณ (`shop_request()`) แต่ละรายการให้รหัสชุด เวอร์ชัน ชื่อ ชนิด และช่วงของรหัสสติกเกอร์ แค็ตตาล็อกแคชไว้ในหน่วยความจำระยะหนึ่ง และถูกล้างถ้าการส่งล้มเหลวด้วยรหัส 32 (USER_NOT_STICKER_OWNER) รหัส ShopException อยู่ใน namespace แยก จึงตั้ง `s->service_code` กลับหลังเรียก

**ชนิดและภาพ**

| ชนิด | `STKOPT` | ไฟล์ภาพ |
| ---- | -------- | ------- |
| STATIC | — | `sticker.png` |
| ANIMATION | `A` | `sticker_animation.png` (PNG เคลื่อนไหว) |
| SOUND | `S` | `sticker.png` |
| ANIMATION_SOUND | `AS` | `sticker_animation.png` |
| POPUP | `P` | `sticker_popup.png` |
| POPUP_SOUND | `PS` | `sticker_popup.png` |

URL ของภาพคือ `https://stickershop.line-scdn.net/stickershop/v1/sticker/<id>/android/<file>` หรือ `…/v2/sticker/<id>/<hash>/android/<file>` เมื่อชุดนั้นมี hash `line_sticker_fetch()` ยอมรับเฉพาะ URL ใต้ต้นทางนั้น สติกเกอร์มีเสียงจะแสดงภาพ แต่ไม่เล่นเสียง

**การส่ง** (`sendSticker`, `line_send_sticker()`): `sendMessage` ด้วยชนิดเนื้อหา 7 **ไม่เข้ารหัส** และ metadata `STKID`, `STKPKGID`, `STKVER`, `STKTXT` (ชื่อชุด) และ `STKOPT` กับ `STKHASH` เมื่อมี การตอบกลับเพิ่มฟิลด์ 21, 22 และ 24 ตามปกติ

**การรับ:** `line_sticker_message_asset()` สร้าง URL ของภาพจาก metadata ของข้อความที่ได้รับ

## อีโมจิ LINE

[`emoji.c`](../../src/emoji.c) อีโมจิ LINE (*sticon*) คือรูปเล็ก ๆ ในข้อความ ระบุด้วย **รหัสผลิตภัณฑ์** (เลขฐานสิบหกตัวพิมพ์เล็ก 24 หลัก) และ **รหัสรายการ** (เลขฐานสิบสามหลัก ตั้งแต่ `001`) `line_emoji_id_valid()` ตรวจทั้งสองรูปแบบก่อนนำไปใช้ใน URL

**แค็ตตาล็อก** (`emojiCatalog`, `line_emoji_catalog_page()`) รวมข้อมูลจากสองแหล่ง

1. **ดัชนีออฟไลน์** `emoji_pack_index.inc`: ชุดสาธารณะ 45 ชุดจากรายการอีโมจิที่ LINE เผยแพร่ พร้อมรหัสผลิตภัณฑ์ ชื่อ เวอร์ชัน และจำนวนรายการ (รวม 9,175 รายการ)
2. **ชุดที่เป็นเจ้าของ**: `getOwnedProductSummaries` บน `/TSHOP4` ด้วยชนิดร้าน `"sticonshop"` หน้าละ 100 หยุดเมื่อได้หน้าว่างหรือหน้าซ้ำ

รายการจริงและป้ายของแต่ละชุดดึงจาก manifest ที่ `https://stickershop.line-scdn.net/sticonshop/v1/product/<product>/android/meta.json` หน้าของแค็ตตาล็อกส่งให้ตัวเลือกเป็นรายการ `{productId, emojiId, label, pack, version, resourceType}` และการค้นหาจับคู่ชื่อชุด หมายเลขรายการ และป้ายที่โหลดมาแล้ว

**รูป:** `https://stickershop.line-scdn.net/sticonshop/v1/sticon/<product>/android/<item>.png` สำหรับภาพนิ่ง และ `<item>_animation.png` สำหรับ PNG เคลื่อนไหว ถ้าแอนิเมชันได้ 404 จะใช้ภาพนิ่งแทน UI ขอด้วย `emoji` (พร้อม `resourceType: "ANIMATION"` สำหรับแบบเคลื่อนไหว)

**ในข้อความ:** ดู [ข้อความ](messaging.md)

## Flex Message

บัญชีทางการส่งการ์ดที่อธิบายด้วย JSON: bubble และ carousel ที่ประกอบด้วย box, text, image, icon, button, separator และ spacer แต่ละตัวมีการกระทำได้ ดู [ข้อมูลอ้างอิง Flex Message ของ LINE](https://developers.line.biz/en/docs/messaging-api/flex-message-elements/)

- **การอ่าน:** ชนิดเนื้อหา 22 การ์ดอยู่ใน `FLEX_JSON` (ไม่เกิน 64 KiB ซ้อนได้จำกัด) และ `ALT_TEXT` `add_flex()` ของ worker ใน `messaging.c` ตรวจแล้วส่งต่อให้ UI
- **การวาด:** [`ui_flex.c`](../../src/ui_flex.c) สร้างต้นไม้การจัดวาง วัดให้พอดีความกว้างของแชต (carousel ตัดขึ้นบรรทัดใหม่) แล้ววาด ถ้าวาดการ์ดไม่ได้จะแสดงข้อความแทน
- **การกระทำ:** `line_flex_link_allowed()` ใน [`flex_asset.c`](../../src/flex_asset.c) ยอมรับลิงก์ HTTPS ลิงก์ LIFF และลิงก์ `line://` ซึ่งเปิดผ่าน [เบราว์เซอร์ในแอป](liff.md)

## Rich message

rich message คือ **imagemap** ของ LINE: รูปหนึ่งรูปที่มีพื้นที่ให้แตะ อาจมีหลายฉาก เป็นชนิดเนื้อหา 17 [`rich_message.c`](../../src/rich_message.c) อ่าน markup แบบเดียวกับที่ LINE สำหรับ Android อ่าน

คีย์ metadata (หรือแบบที่ขึ้นต้นด้วย `RICH_`): `DOWNLOAD_URL` (รูปพื้น) `ALT_TEXT` และ `MARKUP_JSON`

```
canvas  {width, height, initialScene}
images  {name: {x, y, w, h, imageUrl?}}      บริเวณของรูปพื้น
actions {name: {type, text, params}}         web, postback, sendMessage, clipboard, transition, app
scenes  {name: {draws: [{x, y, w, h, image}],
                listeners: [{type: "touch", params: [x, y, w, h], action}],
                video}}
```

`line_rich_parse()` ตรวจทั้งหมดแล้วคืนโครงแบบกะทัดรัดให้ UI

```
{w, h, scene, scenes: {name: {draws: [{x, y, w, h, url, sx, sy, sw, sh, whole}],
                              areas: [{x, y, w, h, kind, value, label}]}}}
```

`kind` ของพื้นที่คือ `link` (URL แบบ HTTPS) `message` (ข้อความที่จะส่งในนามคุณ) `copy` (ข้อความที่จะคัดลอก) หรือ `scene` (เปลี่ยนไปฉากอื่นภายในเครื่อง) รูปพื้นขอที่ `DOWNLOAD_URL/700` (`LINE_RICH_IMAGE_WIDTH`) ตามธรรมเนียม imagemap ของ LINE และตัวดาวน์โหลดถอยไปใช้ `DOWNLOAD_URL` ธรรมดาเมื่อ object store ปฏิเสธความกว้างนั้น ถ้ามีอะไรผิดรูปจะแสดงข้อความแทน

## รูปจากโฮสต์ใดก็ได้

ภาพของ Flex และ rich message มาจาก **โฮสต์ HTTPS ไหนก็ได้** ที่ผู้ส่งเลือก เหมือนบนโทรศัพท์ หมายความว่าเซิร์ฟเวอร์ของผู้ส่งเห็นที่อยู่ IP ของคุณ `line_flex_image_download()` ใน [`flex_asset.c`](../../src/flex_asset.c) ลดความเสี่ยงดังนี้

- HTTPS เท่านั้น คำขอไม่มีคุกกี้หรือข้อมูลยืนยันตัวตน
- callback ของซ็อกเก็ต (`asset_socket()`) ตรวจที่อยู่ที่ resolve ได้ และปฏิเสธที่อยู่ IPv4 และ IPv6 แบบส่วนตัว loopback link-local และที่ไม่ใช่สาธารณะอื่น ๆ ข้อความจึงใช้ไคลเอนต์สอดส่องเครือข่ายในบ้านคุณไม่ได้ สำหรับ IPv6 ผ่านได้เฉพาะ global unicast (2000::/3) ยกเว้นบล็อกเพื่อการใช้งานพิเศษของ IANA ที่อยู่ข้างใน ได้แก่ 2001::/23 (Teredo การทดสอบสมรรถนะ และการกำหนดของโปรโตคอลอื่น) 2001:db8::/32 และ 3fff::/20 (สำหรับเอกสาร) และ 2002::/16 (6to4)
- ตาม redirect เฉพาะไปยัง URL แบบ HTTPS ที่อนุญาต
- ยอมรับเฉพาะ JPEG, PNG หรือ WebP ไม่เกิน 64 MiB ซึ่งเป็นขีดจำกัดเพื่อกันเซิร์ฟเวอร์ที่ส่งไม่หยุดเท่านั้น
- โฮสต์ที่ตอบ 429 หรือ 503 จะไม่ได้รับคำขออีกจนกว่า `Retry-After` จะหมด และจะข้ามการลอง URL ธรรมดาของ imagemap
- `LINE_DEBUG_MEDIA=1` รายงานขั้นตอนที่ล้มเหลว หมายเลขครั้งที่ลอง รหัสของ curl สถานะ HTTP และจำนวนไบต์ที่รับ โดยละเว้น URL ของรูปและข้อความจากเซิร์ฟเวอร์

## อัลบั้ม โน้ต และโพล

[`shared_content.c`](../../src/shared_content.c) ส่วนนี้ไม่ใช่บริการ Thrift แต่เป็น REST endpoint บน `legy.line-apps.com` ที่ใช้ **channel token** ยืนยันสิทธิ์

1. `approveChannelAndIssueChannelToken` บน `/CH4` ด้วย `{1: channel ID}` โดย channel คือ `1375220249` สำหรับอัลบั้ม และ `1341209850` สำหรับโน้ต ฟิลด์ 5 ของคำตอบคือโทเค็น แคชไว้ตลอดเซสชัน
2. คำขอมี `x-line-access`, `x-line-channeltoken`, `x-line-mid` (MID ของคุณ หรือแชตสำหรับคำขอเกี่ยวกับอัลบั้ม) `x-line-chat-id` หรือ `x-line-album` และ `x-lhm` ที่บอกเมธอด HTTP เชิงตรรกะ

| เมธอดของ worker | Endpoint |
| --------------- | -------- |
| `sharedList` (อัลบั้ม) | `GET /ext/album/moa/v2/albums?cursor=…&orderBy=createTimeDesc` |
| `sharedPhotos` | `GET /ext/album/api/v6/albums/<album>/photos?cursor=…&pageSize=50&orderBy=createTimeDesc&include=all` |
| `sharedPhoto` | `GET /oa/r/album/a/<photo>` คืนรูปเป็น media lease |
| `sharedList` (โน้ต) | `GET /ext/note/nt/api/v57/post/list.json?homeId=<chat>&sourceType=TALKROOM` |
| `sharedPost` | `POST /ext/note/nt/api/v57/post/create.json?homeId=<chat>&sourceType=TALKROOM` |
| ค้นหาโพล | `/mh/api/v46/search/note.json` |

ไคลเอนต์ดูอัลบั้มและเปิดรูป อ่านและโพสต์โน้ตข้อความ และแสดงรายการโพลที่โน้ตอ้างถึงได้ การจัดการอัลบั้ม การแก้โน้ต และการโหวตยังไม่ได้ทำ
