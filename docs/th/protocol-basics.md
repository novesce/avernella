# พื้นฐานโปรโตคอล LINE

[สารบัญ](README.md) · ก่อนหน้า: [ส่วนติดต่อผู้ใช้](ui.md) · ถัดไป: [การลงชื่อเข้าใช้](sign-in.md)

หน้านี้อธิบายว่าคำขอ *ทุกตัว* ที่ส่งไปหา LINE ถูกสร้าง ส่ง และตอบกลับอย่างไร หน้าโปรโตคอลหลังจากนี้ถือว่าคุณรู้เรื่องในหน้านี้แล้ว

LINE ไม่ได้เผยแพร่เอกสารเรื่องนี้ ข้อมูลทั้งหมดได้จากการแกะ LINE สำหรับ Android 26.16.0 แล้วตรวจทานกับโปรเจกต์โอเพนซอร์สที่ทำงานร่วมกับ LINE (ดู [การมีส่วนร่วมพัฒนา](contributing.md)) ไคลเอนต์แสดงตัวเป็น **แอป LINE บน Windows** เวอร์ชัน 9.7.0.3556

## เซิร์ฟเวอร์

| โฮสต์ | ใช้ทำอะไร | โค้ด |
| ----- | --------- | ---- |
| `legy.line-apps.com` | RPC ทุกตัว: ลงชื่อเข้าใช้ sync ข้อความ รายชื่อ กลุ่ม การโทร LIFF ร้านค้า channel token อัลบั้มและโน้ต | `transport.c`, `shared_content.c` |
| `obs.line-apps.com` | object storage: ไฟล์แนบ ภาพตัวอย่าง การอัปโหลดรูปโปรไฟล์ | `obs_http.c` |
| `profile.line-scdn.net` | รูปโปรไฟล์ (สาธารณะ) | `line_avatar_download()` |
| `stickershop.line-scdn.net` | ภาพสติกเกอร์และอีโมจิ และ manifest ของชุด (สาธารณะ) | `stickers.c`, `emoji.c`, `line_public_asset()` |
| โฮสต์ HTTPS สาธารณะใดก็ได้ | รูปใน Flex และ rich message ที่ผู้ส่งเลือก | `flex_asset.c` |
| relay การโทร (ที่อยู่มาจาก call route) | การส่งสัญญาณและมีเดียของการโทรผ่าน UDP | `call.c` |

## พาธของบริการ

RPC ทุกตัวคือ HTTP POST ไปที่ `https://legy.line-apps.com<path>` โดยพาธเป็นตัวเลือกบริการ

| พาธ | บริการ | เมธอดที่ไคลเอนต์เรียก |
| --- | ------ | --------------------- |
| `/S4` | TalkService | `getProfile`, `getMessageBoxes`, `getChats`, `getAllChatMids`, `getRecentMessagesV2`, `getPreviousMessagesV2WithRequest`, `sendMessage`, `sendChatChecked`, `react`, `unsendMessage`, `createChat`, `updateChat`, `inviteIntoChat`, `cancelChatInvitation`, `acceptChatInvitation`, `rejectChatInvitation`, `deleteOtherFromChat`, `deleteSelfFromChat`, `reissueChatTicket`, `getContactsV2`, `getFriendRequests`, `removeFriendRequest`, `updateProfileAttributes`, `getUserTicket`, `negotiateE2EEPublicKey`, `getE2EEPublicKey`, `getLastE2EEGroupSharedKey`, `getE2EEGroupSharedKey`, `getChatRoomAnnouncements`, `createChatRoomAnnouncement`, `removeChatRoomAnnouncement`, `acquireEncryptedAccessToken`, `getLastOpRevision` |
| `/RE4` | RelationService | `getContactsV3`, `getUserFriendIds`, `getUserRecommendIds`, `findContactBySearchIdOrTicketV3`, `findContactByPhoneV3`, `addFriendByMid`, `updateFriendFavorite`, `updateFriendHidden`, `updateUserFriendOverridenName`, `blockRelation`, `unblockRelation` |
| `/SYNC4` | SyncService | `sync` |
| `/AS4` | AuthService | `logoutV2` |
| `/acct/lgn/sq/v1` | การเข้าสู่ระบบด้วย QR สำหรับอุปกรณ์รอง | `createSession`, `createQrCodeForSecure`, `verifyCertificate`, `createPinCode`, `qrCodeLoginV2ForSecure` |
| `/acct/lp/lgn/sq/v1` | ฝั่ง long poll ของการเข้าสู่ระบบด้วย QR | `checkQrCodeVerified`, `checkPinCodeVerified` |
| `/api/v3/TalkService.do` | การเข้าสู่ระบบด้วยอีเมล ขั้นที่ 1 (Thrift แบบ **Binary**) | `getRSAKeyInfo` |
| `/api/v3p/rs` | การเข้าสู่ระบบด้วยอีเมล (Thrift แบบ **Binary**) | `loginV2`, `confirmE2EELogin` |
| `/LF1` | รอการยืนยันจากโทรศัพท์ (JSON ไม่ใช่ Thrift) | — |
| `/EXT/auth/tokenrefresh/v1` | การต่ออายุโทเค็น | `refresh` |
| `/TSHOP4` | ShopService | `getOwnedProductSummaries` |
| `/CH4` | ChannelService | `approveChannelAndIssueChannelToken` |
| `/V4` | CallService | `acquireCallRoute` |
| `/LIFF1` | LiffService | `issueLiffView` |

อัลบั้มและโน้ตเป็น REST endpoint ใต้ `legy.line-apps.com/ext/…` ที่ใช้ channel token อธิบายในหน้า [มีเดีย](media.md)

## การเข้ารหัส Thrift แบบ Compact

RPC ของ LINE คือการเรียก [Apache Thrift](https://thrift.apache.org/) ด้วย **Compact protocol** [`thrift.c`](../../src/thrift.c) เขียนเฉพาะส่วนที่ LINE ใช้ ราว 600 บรรทัด โดยไม่ใช้ไลบรารี Thrift

### คำขอบนสาย

`line_encode_call(method, args)` เขียนข้อมูลดังนี้

| ไบต์ | ความหมาย |
| ---- | -------- |
| `0x82` | รหัสของ Compact protocol |
| `0x21` | เวอร์ชัน 1 (5 บิตล่าง) และชนิดข้อความ CALL = 1 (3 บิตบน) |
| varint `0` | sequence ID เป็น 0 เสมอ เพราะหนึ่งคำขอต่อหนึ่งการแลกเปลี่ยน HTTP |
| ความยาวแบบ varint + ไบต์ | ชื่อเมธอด เช่น `sendMessage` |
| struct | struct อาร์กิวเมนต์ของเมธอด (`<method>_args`) |

**struct** คือรายการฟิลด์ตามด้วยไบต์หยุด `0x00` แต่ละฟิลด์เริ่มด้วยเฮดเดอร์

- ถ้าหมายเลขฟิลด์มากกว่าฟิลด์ก่อนหน้า 1 ถึง 15 ไบต์เดียวเก็บได้ทั้งคู่: `(delta << 4) | type`
- ถ้าไม่ใช่ ไบต์ชนิดจะตามด้วยหมายเลขฟิลด์แบบ zigzag varint

| ชนิด | รหัส Compact | การเข้ารหัสค่า |
| ---- | ------------ | -------------- |
| bool | 1 (true) / 2 (false) | ในฟิลด์ ค่า *คือ* ชนิดเลย ในลิสต์ใช้หนึ่งไบต์ |
| byte | 3 | หนึ่งไบต์ |
| i16, i32, i64 | 4, 5, 6 | zigzag varint |
| double | 7 | 8 ไบต์ little-endian |
| binary / string | 8 | ความยาวแบบ varint ตามด้วยไบต์ |
| list, set | 9, 10 | หนึ่งไบต์ `(count << 4) | ชนิดสมาชิก` ถ้า count เป็น 15 แปลว่ามีจำนวนแบบ varint ตามมา แล้วจึงเป็นสมาชิก |
| map | 11 | จำนวนแบบ varint แล้ว (ถ้าไม่เป็นศูนย์) หนึ่งไบต์ `(ชนิดคีย์ << 4) | ชนิดค่า` แล้วคีย์ ค่า คีย์ ค่า… |
| struct | 12 | ฟิลด์ทั้งหมด แล้ว `0x00` |

### คำตอบบนสาย

คำตอบเริ่มด้วย `0x82` ตามด้วย `0x41` (REPLY) หรือ `0x61` (EXCEPTION) sequence ID และชื่อเมธอดซึ่งต้องตรงกัน `line_decode_reply()` ตรวจโปรโตคอล ชนิดข้อความ และชื่อเมธอด เนื่องจากแต่ละคำขอใช้การแลกเปลี่ยน HTTPS ของตัวเอง ไคลเอนต์จึงยอมรับ sequence ID ใด ๆ เช่นเดียวกับไคลเอนต์ Thrift อื่น จากนั้นเป็น **result struct**

- **ฟิลด์ 0** คือค่าที่สำเร็จ ความสำเร็จของเมธอด `void` คือ struct ว่าง
- **ฟิลด์ 1** คือ exception ของบริการ โดยทั่วไปเป็น `TalkException` ซึ่งฟิลด์ 1 เป็นรหัสข้อผิดพลาดและฟิลด์ 2 เป็นข้อความ บางบริการเพิ่ม payload ในฟิลด์ 3

ข้อความแบบ EXCEPTION (`0x61`) คือ application exception ของ Thrift เช่น "unknown method" จะกลายเป็น `LINE_METHOD_UNAVAILABLE` (ชนิด 1) หรือข้อผิดพลาดทั่วไป

### รูปแบบ JSON ที่โค้ดใช้

แทนที่จะใช้ struct ที่สร้างจากเครื่องมือ โค้ดสร้างและอ่าน Thrift เป็น **อ็อบเจกต์ของ json-c** นี่คือธรรมเนียมสำคัญที่สุดในโค้ดโปรโตคอล

**การเขียน** คีย์ของคำขอมีรูปแบบ `"<หมายเลขฟิลด์>:<ชนิด>"` และตัวเข้ารหัสเอาชนิดจากคีย์

```c
g_autoptr(J) args = obj();             /* sendMessage_args */
J *msg = obj();                         /* Message */
number(args, "1:i32", ++s->seq);        /* ฟิลด์ 1: ลำดับคำขอ */
put(args, "2:obj", msg);                /* ฟิลด์ 2: ตัวข้อความ */
put(msg, "2:str", string(chat));        /* Message.to */
number(msg, "15:i32", 0);               /* Message.contentType = NONE (ข้อความ) */
put(msg, "18:map:str:str", meta);       /* Message.contentMetadata */
put(msg, "20:list:str", chunks);        /* Message.chunks */
```

(`number()`, `put()` และ `string()` เป็นฟังก์ชันช่วยแบบ static ใน `messaging.c`)

| ชนิดในคีย์ | ชนิด Thrift |
| ---------- | ----------- |
| `bool` | bool |
| `byte`, `i16`, `i32`, `i64` | จำนวนเต็ม |
| `double` | double |
| `str` | string หรือ binary ข้อมูล binary ใส่ด้วย `json_object_new_string_len()` จึงมีไบต์ NUL ได้ |
| `obj` | struct |
| `list:<type>`, `set:<type>` | list หรือ set ของชนิดนั้น เช่น `list:str`, `set:str` |
| `map:str:<type>`, `map:i32:<type>` | map ที่คีย์เป็น string หรือ i32 |

ฟิลด์ถูกเขียนตามลำดับที่เพิ่มเข้าไป ไม่จำเป็นต้องเรียงจากน้อยไปมาก เพราะตัวเข้ารหัสจะใช้เฮดเดอร์ฟิลด์แบบยาวเมื่อจำเป็น

**การอ่าน** struct ที่ถอดรหัสแล้วเป็นอ็อบเจกต์ JSON ที่ใช้ **เฉพาะหมายเลขฟิลด์ฐานสิบ** เป็นคีย์ ไม่มีชนิด เช่น `"1"`, `"4"`, `"20"` list กลายเป็นอาร์เรย์ map กลายเป็นอ็อบเจกต์ (คีย์ถูกแปลงเป็น string) binary กลายเป็น string ที่อาจมีไบต์ NUL (อ่านด้วย `json_object_get_string_len()`) ฟังก์ชันช่วยใน [`line.h`](../../src/line.h) อ่านได้อย่างปลอดภัย

```c
const char *id   = strfield(message, "4");          /* NULL ถ้าไม่ใช่ string */
J *meta          = field(message, "18");            /* NULL ถ้าไม่มี */
gint64 type      = json_object_get_int64(field(message, "15"));
```

`field()` คืน `NULL` ถ้าไม่มีฟิลด์นั้นหรือถ้าตัวแม่ไม่ใช่อ็อบเจกต์ การเรียกต่อกันอย่าง `field(field(op, "20"), "18")` จึงปลอดภัย

**ขีดจำกัด** ตัวถอดรหัสปฏิเสธการซ้อนลึกเกิน 48 ชั้น ค่ารวมเกิน 300,000 ค่า หมายเลขฟิลด์ซ้ำ และไบต์ที่เกินมาหลัง struct คีย์ของ map ต้องเป็นค่าสเกลาร์ ตัวถอดรหัสปฏิเสธคีย์ซ้ำและคีย์สตริงที่มีไบต์ NUL ก่อนเพิ่มค่าลงในออบเจ็กต์ JSON ส่วนค่าของ map ยังใช้ list, struct และ map ได้

### Thrift แบบ Binary ใช้เฉพาะการเข้าสู่ระบบด้วยอีเมล

endpoint ของการเข้าสู่ระบบด้วยอีเมล (`/api/v3/TalkService.do` และ `/api/v3p/rs`) ใช้ **Binary protocol** รุ่นเก่า `line_encode_auth()` และ `line_decode_auth()` จัดการรูปแบบนี้: คำบอกเวอร์ชัน 4 ไบต์ `0x80010001` (CALL) ชื่อเมธอดพร้อมความยาว 4 ไบต์ sequence ID 4 ไบต์ แล้วเป็นฟิลด์ที่มีชนิด 1 ไบต์ หมายเลข 2 ไบต์แบบ big-endian และค่าที่มีความกว้างคงที่ รองรับเฉพาะชนิดที่เมธอดเหล่านั้นใช้ (bool, i32, string, struct) คำตอบมี `0x80010002` (REPLY) หรือ `0x80010003` (EXCEPTION)

## การแลกเปลี่ยน HTTP

`exchange()` ใน [`transport.c`](../../src/transport.c) ส่ง RPC ทุกตัว มีฟังก์ชันสาธารณะที่ห่อมันไว้ดังนี้

| ฟังก์ชัน | โปรโตคอล | ใช้กับ |
| -------- | -------- | ------ |
| `line_rpc(s, path, method, args, &error)` | Compact ใช้ access token ของเซสชัน หมดเวลาใน 30 วินาที และลองใหม่หนึ่งครั้งหลังต่ออายุโทเค็นที่หมดอายุ | เกือบทุกอย่าง |
| `line_call(s, path, method, args, access, poll_seconds, &error)` | Compact ระบุโทเค็นเอง (หรือไม่มี) และเวลา long poll ได้ | การจับคู่ sync การต่ออายุโทเค็น |
| `line_auth_call(s, path, method, args, &error)` | Binary ไม่มีโทเค็น | การเข้าสู่ระบบด้วยอีเมล |
| `line_auth_wait(s, verifier, &error)` | JSON GET บน `/LF1` นาน 180 วินาที | รอการยืนยันจากโทรศัพท์ระหว่างเข้าสู่ระบบด้วยอีเมล |

### เฮดเดอร์ของคำขอ

```
POST /S4 HTTP/2
Host: legy.line-apps.com
x-line-application: DESKTOPWIN	9.7.0.3556	WINDOWS	10.0.0-NT-x64
x-lal: <locale>                        (th_TH, ja_JP…; en_US for C/POSIX)
x-lpv: 1
x-lhm: POST
x-line-access: <access token>          (เมื่อลงชื่อเข้าใช้แล้ว)
x-lst: <มิลลิวินาที>                    (เฉพาะ long poll)
Accept: application/x-thrift
Content-Type: application/x-thrift
Accept-Encoding: gzip
User-Agent: Line/9.7.0.3556
```

ฟิลด์ของ `x-line-application` คั่นด้วยอักขระแท็บ `LINE_APP_HEADER` และ `LINE_USER_AGENT` ใน `line.h` เก็บตัวตนของไคลเอนต์ไว้ที่เดียว ทุกคำขอ (RPC, object storage, อัลบั้มและโน้ต) จึงระบุเวอร์ชันไคลเอนต์เดียวกัน `x-lal` คือ locale ของเดสก์ท็อปจาก `line_language_tag()` ใน `line.h` ซึ่งเป็นแท็กเดียวกับที่ LIFF ส่งเป็น `lang` เดสก์ท็อปภาษาไทยจึงส่ง `th_TH` ทุกที่ เหมือนไคลเอนต์ของ LINE เอง `x-lst` บอก LINE ว่าถือคำขอ long poll ไว้ได้นานเท่าไร การรอยืนยันบน `/LF1` ใช้ `x-lhm: GET` และ `Accept: application/json` แทน

### กฎของการขนส่ง

- **HTTPS เท่านั้น** ไม่ตาม redirect หมดเวลาการเชื่อมต่อใน 10 วินาที และ body ใหญ่ได้ไม่เกิน 16 MiB
- **หนึ่งพูลการเชื่อมต่อต่อหนึ่งเธรด** แต่ละเธรดเก็บ curl handle ของตัวเองใน thread-local storage ล้างตัวเลือกหลังการโอนแต่ละครั้ง และใช้การเชื่อมต่อซ้ำ long poll ของเธรด sync จึงไม่ขวาง RPC
- **แคชที่ใช้ร่วม** คำตอบ DNS และ TLS session ใช้ร่วมกันข้ามเธรด ([`http_tune.c`](../../src/http_tune.c)) ใช้ TLS 1.2 ขึ้นไป ตรวจใบรับรองและชื่อโฮสต์ ต่อรอง HTTP/2 เปิด TCP Fast Open และ keepalive
- **การตรวจ revision แบบสั้น** `getLastOpRevision` หมดเวลาใน 1.8 วินาที เพราะเป็นแค่การตรวจเร็ว ๆ

### โทเค็นที่ถูกหมุนเวียน

คำตอบอาจมีเฮดเดอร์ `x-line-next-access: <token>` ซึ่งหมายความว่า LINE หมุนเวียน access token `exchange()` จะรับโทเค็นใหม่ก็ต่อเมื่อ: คำขอไม่ใช่การต่ออายุ ใช้โทเค็นปัจจุบันของเซสชัน คำตอบเป็นคำตอบจริง (สำเร็จหรือ service exception) มีโทเค็นที่พิมพ์ได้และถูกต้องเพียงหนึ่งตัว และต่างจากโทเค็นเดิม จากนั้นจะตั้ง `s->token_changed` แล้ว worker บันทึกเซสชันหลังคำขอนั้น เธรด sync ส่งต่อการหมุนเวียนที่เห็นให้ worker ในชุดข้อมูลของมัน

## ข้อผิดพลาด

`exchange()` และตัวถอดรหัสแปลงความล้มเหลวทุกแบบเป็น `GError` ในโดเมน `LINE_ERROR` รหัสอยู่ใน [`line.h`](../../src/line.h)

| รหัส | ความหมาย | สาเหตุที่พบบ่อย |
| ---- | -------- | --------------- |
| `1` | ล้มเหลวทั่วไป | service exception คำตอบผิดรูป ข้อผิดพลาด HTTP |
| `LINE_TIMEOUT` (2) | หมดเวลา | เครือข่าย หรือ long poll ที่ว่าง รวมถึง HTTP 408, 504 และ 410 ระหว่างรอยืนยัน |
| `LINE_METHOD_UNAVAILABLE` (3) | LINE ไม่เปิดเมธอดนี้ที่นี่ | "api method not capable", "unknown method" |
| `LINE_SESSION_EXPIRED` (4) | ใช้เซสชันต่อไม่ได้ ต้องจับคู่ใหม่ | โทเค็นหมดอายุและการต่ออายุถูกปฏิเสธ |
| `LINE_OBS_UNAUTHORIZED` (5) | โทเค็นของ object storage ถูกปฏิเสธ | ทิ้งแล้วขอใหม่ครั้งถัดไป |
| `LINE_ASSET_NOT_FOUND` (6) | HTTP 404 จาก CDN หรือ OBS | ไม่มีภาพตัวอย่างหรือแอนิเมชัน |
| `LINE_OWN_KEY_MISSING` (7) | อุปกรณ์นี้ไม่มีกุญแจ Letter Sealing รุ่นที่ต้องใช้ | ข้อความเก่าบนอุปกรณ์ใหม่ |
| `LINE_RETRY` (8) | เซสชันที่บันทึกไว้ติดต่อ LINE ไม่ได้ | ออฟไลน์ตอนเปิดแอป: เก็บเซสชันไว้แล้วลองใหม่ |
| `LINE_RATE_LIMITED` (9) | LINE ขอให้รอ | HTTP 429 หรือ 503, TalkException 4, 58 หรือ 101 หรือโฮสต์ที่ยังอยู่ในช่วงพัก |
| `LINE_ACCESS_DENIED` (10) | LINE ปฏิเสธข้อมูลยืนยันตัวตนของคำขอ | HTTP 401 หรือ 403 |

แยกต่างหาก `s->service_code` เก็บ **หมายเลขข้อผิดพลาดของ LINE เอง** จากฟิลด์ 1 ของ exception ในคำตอบล่าสุด หรือ -1 ผู้เรียกตรวจค่านี้สำหรับกรณีเฉพาะ รหัส TalkException ที่ไคลเอนต์ใช้

| รหัส | ที่ไหน | ความหมายสำหรับไคลเอนต์ |
| ---- | ------ | ---------------------- |
| 119 | การเรียก `/S4` ใดก็ได้ | access token หมดอายุ: ต่ออายุแล้วลองใหม่ (`line_rpc()`) |
| 1, 7, 8, 14, 17 | `getProfile`, `getLastOpRevision` | เซสชันถูกปฏิเสธ: ลืมเซสชันนั้น (`session_rejected()` ใน `main.c`) |
| 1000 | `/EXT/auth/tokenrefresh/v1` | ใช้ refresh token ไม่ได้แล้ว: ออกจากระบบ |
| 1001 | `/EXT/auth/tokenrefresh/v1` | ลองใหม่ภายหลัง |
| 18, 19, 21, 23 | `loginV2` | อีเมลหรือรหัสผ่านไม่ถูกต้อง |
| 1, 2 | `verifyCertificate` | ไม่มีใบรับรอง: ต้องใช้ PIN |

บริการอื่นอย่าง RelationService, ShopService และ endpoint ต่ออายุโทเค็นมีหมายเลขรหัสเป็นของ **ตัวเอง** โค้ดที่เรียกบริการเหล่านี้จะตั้ง `s->service_code` กลับเป็น -1 หลังเรียก เพื่อไม่ให้รหัสของ Relation ถูกเข้าใจผิดว่าเป็นรหัสของ Talk (ดู `relation()` ใน `friends.c`)

## การจำกัดอัตรา

ทุกเส้นทาง HTTP ที่ไปถึง LINE ใช้ตาราง **ช่วงพักแยกตามโฮสต์** ร่วมกัน (`line_cooldown_*()` ใน [`http_tune.c`](../../src/http_tune.c)) ได้แก่ RPC ใน `transport.c`, object storage และ CDN ใน `obs_http.c`, อัลบั้มและโน้ตใน `shared_content.c` และภาพของการ์ดใน `flex_asset.c`

- **การเริ่มช่วงพัก** HTTP 429 หรือ 503 ตั้งช่วงพักให้โฮสต์นั้นตาม `Retry-After` หรือ 30 วินาทีถ้าไม่มี (`line_cooldown_note()`) TalkException `EXCESSIVE_ACCESS` (4), `CONGESTION_CONTROL` (58) หรือ `SERVER_BUSY` (101) บน `/S4` หรือ `/SYNC4` ตั้ง 30 วินาทีให้ `legy.line-apps.com`
- **การเคารพช่วงพัก** ก่อนหมดช่วงพัก คำขอทุกตัวไปยังโฮสต์นั้นจะล้มเหลวทันทีด้วย `LINE_RATE_LIMITED` โดยไม่แตะเครือข่ายเลย (`line_cooldown_check()`) โฮสต์อื่นทำงานต่อได้ object storage ที่ยุ่งจึงไม่ทำให้แชตหยุด
- **เมื่อเปิดแอปอีกครั้ง** ตัว worker เก็บเวลาสิ้นสุดการรอของโฮสต์ RPC หลักไว้ในไฟล์ `rpc.retry-until` ที่อ่านได้เฉพาะเจ้าของ ไฟล์นี้มีเพียงเวลา การเข้าสู่ระบบใหม่ การกู้คืนเซสชัน และคำขอเบื้องหลังจะรอจนถึงเวลานั้น ระหว่างรอยังออกจากระบบบนเครื่องได้
- **ใครต้องรอ** ตัวจับเวลาบำรุงรักษา (`sync_revision()`) และเธรด long poll รออย่างน้อยเท่ากับช่วงพักของ legy (`line_transport_retry_after()`) ไม่มีอะไรส่งคำขอที่ถูกปฏิเสธซ้ำเองอัตโนมัติ ผู้ใช้ลองใหม่ได้เมื่อหมดเวลาแล้ว
- **ไม่ "ลองทางอื่นแทน"** ภาพ imagemap ที่ได้ 429 จะไม่ถูกตามด้วยการลอง URL ธรรมดาแทน (`line_flex_image_download()`)

ทั้งหมดนี้ทำตาม LINE สำหรับ Android 26.x: interceptor ของ HTTP (`ti0.d`) ลอง 429 ซ้ำเฉพาะเมื่อ `Retry-After` น้อยกว่า 5 วินาที และไคลเอนต์ object storage (`a14.g`) ลองซ้ำครั้งเดียว และเฉพาะหลัง 401 หรือ 503

## เมื่อ LINE ปฏิเสธเซสชัน

LINE สำหรับ Android ออกจากระบบอุปกรณ์ที่เชื่อมทันทีเมื่อได้ TalkException `AUTHENTICATION_FAILED` (1), `NOT_AVAILABLE_USER` (7) หรือ `NOT_AUTHORIZED_DEVICE` (8) ครั้งแรก (`ac5.e`, `ac5.b`) และหลังจากนั้นปฏิเสธในเครื่องที่จะส่งอะไรด้วยโทเค็นนั้นอีก (`getClient()` ในไคลเอนต์ Thrift ของมัน) Avernella ทำแบบเดียวกัน

- **ตัวล็อก** เมื่อคำขอ `/S4` หรือ `/SYNC4` ที่ใช้โทเค็นของเซสชันเองได้รหัสใน `line_auth_rejected()` (1, 7, 8, 14, 17) หรือ refresh token ถูกปฏิเสธ `exchange()` จะตั้ง `s->denied` หลังจากนั้นคำขอใดที่ใช้โทเค็นนั้นจะล้มเหลวในเครื่องด้วย `LINE_SESSION_EXPIRED` การเรียกระหว่างจับคู่ซึ่งใช้โทเค็นชั่วคราวของตัวเองไม่ได้รับผลกระทบ
- **การออกจากระบบ** หลังคำขอแต่ละครั้ง `handle()` ใน `main.c` เห็นตัวล็อกแล้วเรียก `end_session()`: ลืมเซสชัน ส่ง `signed_out` และพาหน้าต่างกลับไปหน้าลงชื่อเข้าใช้ หน้าต่างไม่ลงชื่อเข้าใช้ใหม่เองเด็ดขาด
- **long poll** เธรด `line-sync` มีสำเนาโทเค็นของตัวเอง เมื่อถูกปฏิเสธ หรือได้ HTTP 401 หรือ 403 มันจะรายงาน `denied` ครั้งเดียวแล้วหยุด poll worker ยืนยันครั้งเดียวด้วย `checked_profile()`: เซสชันที่ยังใช้ได้จะเริ่ม long poll ใหม่ เซสชันที่ถูกปฏิเสธจะออกจากระบบ ส่วนกรณีอื่นจะรอ 15 วินาที (เครือข่าย) หรือ 5 นาที (คำตอบอื่นจาก LINE) ก่อนตรวจครั้งถัดไป
- **HTTP 401 หรือ 403** แบบเดียวกับ interceptor ของ LINE `checked_profile()` ต่ออายุโทเค็นครั้งเดียวแล้วถามอีกครั้งเดียว ไม่วนซ้ำ

## การต่ออายุโทเค็น

LINE ออก access token มาพร้อมตัวเลขสองตัว คือ `durationUntilRefreshInSec` และ `tokenIssueTimeEpochSec` `line_schedule_refresh()` เก็บผลรวมเป็น `s->refresh_at` และบันทึกไปกับเซสชัน ค่าที่ไม่สมเหตุสมผลจะถูกเพิกเฉย ได้แก่ ระยะเวลาน้อยกว่าหนึ่งนาทีหรือมากกว่าสิบปี และเวลาออกที่ย้อนไปเกินหนึ่งปีหรือล่วงหน้าเกินหนึ่งวัน

การต่ออายุเกิดได้สองทาง

1. **ตามกำหนด** `maintain_tokens()` ใน `main.c` ซึ่งถูกเรียกจากตัวจับเวลาบำรุงรักษา จะเรียก `line_refresh_session()` เมื่อ `line_refresh_due()` บอกว่าถึงเวลา ถ้าไม่รู้กำหนด (เซสชันเก่าที่บันทึกไว้) จะต่ออายุทันที ถ้าล้มเหลวจะลองใหม่แบบเว้นระยะทวีคูณ (`line_retry_delay()`) สูงสุดชั่วโมงละครั้ง และไม่ออกจากระบบเอง
2. **เมื่อจำเป็น** เมื่อ `line_rpc()` ใดล้มเหลวด้วย TalkException 119 จะเรียก `refresh_access()` หนึ่งครั้งแล้วส่งคำขอเดิมซ้ำด้วยอาร์กิวเมนต์เดิม สำหรับการส่งข้อความ หมายถึงใช้ลำดับคำขอเดิมและ chunk ที่เข้ารหัสแล้วชุดเดิม

`refresh_access()` เรียก `refresh` บน `/EXT/auth/tokenrefresh/v1` ด้วย `{1: {1: refresh token}}` คำตอบมี access token ใหม่ในฟิลด์ 1 ระยะเวลาในฟิลด์ 2 เวลาออกในฟิลด์ 4 และอาจมี refresh token ใหม่ในฟิลด์ 5 ถ้าไม่มีระยะเวลาให้มา จะต่ออายุทุกวันเพื่อให้ refresh token ถูกใช้งานอยู่เสมอ

มีแค่การปฏิเสธอย่างชัดแจ้งที่ยุติเซสชัน ได้แก่ รหัส 1000 จากการต่ออายุ หรือรหัส 119 ที่ยังเกิดหลังต่ออายุแล้ว ข้อผิดพลาดของเครือข่ายจะเก็บเซสชันไว้
