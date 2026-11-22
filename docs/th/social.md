# เพื่อน โปรไฟล์ และกลุ่ม

[สารบัญ](README.md) · ก่อนหน้า: [มีเดีย](media.md) · ถัดไป: [LIFF](liff.md)

ด้านผู้คนของ LINE: การค้นหาและเพิ่มเพื่อน การจัดการเพื่อน โปรไฟล์ คำขอเป็นเพื่อน และกลุ่ม เพื่อนและโปรไฟล์อยู่ใน [`friends.c`](../../src/friends.c) กลุ่มอยู่ใน [`groups.c`](../../src/groups.c) และส่วนเชื่อมของ worker อยู่ใน [`main.c`](../../src/main.c) (`friend_directory()`, `find_friend()`, `add_friend()`, `friend_action()`, `profile_card()`, `profile_action()`, `friend_requests()`, `answer_request()`, `change_group()`, `pending_groups()`, `answer_group()`)

## สองบริการ

การจัดการเพื่อนส่วนใหญ่ไปที่ **RelationService** บน `/RE4` ส่วนโปรไฟล์ กลุ่ม และคำขอเป็นเพื่อนไปที่ **TalkService** บน `/S4`

ความล้มเหลวของ RelationService มีชุดรหัสข้อผิดพลาดเป็นของตัวเอง `relation()` ใน `friends.c` แปลงรหัสที่พบบ่อยเป็นข้อความที่อ่านเข้าใจ แล้วตั้ง `s->service_code` กลับ เพื่อไม่ให้ถูกเข้าใจผิดว่าเป็นรหัสของ TalkService

| รหัส | ข้อความที่แสดง |
| ---- | -------------- |
| 1 | LINE could not find an available account for this request. |
| 2 | LINE requires age verification for this account to use friend search. |
| 3 | LINE's friend limit has been reached. |
| 4 | LINE temporarily limited friend requests. Try again later. |
| 5 | LINE declined the friend request data. |
| 6 | LINE requires this person's QR invitation for this request. |

## รายชื่อเพื่อน

`line_friends_page()` และ `line_managed_friends_page()`

1. **รหัส:** `getUserFriendIds` บน `/RE4` ด้วย `{1: {1: cursor, 2: 2}}` (2 = ไม่ถูกบล็อก) มุมมองที่บล็อกใช้ 1 และมุมมองคำแนะนำใช้ `getUserRecommendIds` ฟิลด์ 1 ของคำตอบคือ MID หนึ่งหน้า ฟิลด์ 2 คือ cursor ถัดไป worker เป็นผู้ถือ cursor UI บอกแค่ "หน้าถัดไป"
2. **รายละเอียด:** `getContactsV3` บน `/RE4` ด้วย `{1: {1: [{1: mid}, …]}}` ถอยไปใช้ `getContactsV2` บน `/S4` สำหรับเซิร์ฟเวอร์รุ่นเก่า
3. **แถว:** `{id, profileName, overrideName, avatarKey, favorite, hidden, blocked, canAdd, official, recommended}` เพื่อนที่ซ่อนและบล็อกไม่อยู่ในรายชื่อหลัก แต่แสดงใต้ **Manage**

## การค้นหาและเพิ่มเพื่อน

**การค้นหา** (`findFriend`, `line_friend_lookup()`) รับอินพุตสามแบบ และ `search_value()` ตรวจแต่ละแบบอย่างเข้มงวด

| แบบ | ตัวอย่าง | RPC บน `/RE4` |
| --- | -------- | ------------- |
| LINE ID | `alice.1` | `findContactBySearchIdOrTicketV3` ด้วย `{1: {1: {1: "alice.1"}}}` |
| ลิงก์ส่วนตัวหรือ ticket | `https://line.me/ti/p/AbC_1-xy`, `line://ti/p/…` | เมธอดเดียวกัน โดยใส่ ticket ในฟิลด์ 2: `{1: {1: {2: ticket}}}` ลิงก์แบบ `~id` ถือเป็น LINE ID |
| เบอร์โทร | `+12025550123` (รูปแบบสากล) | `findContactByPhoneV3` ด้วย `{1: {1: "+12025550123"}}` |

worker จำผลลัพธ์ไว้ (`lookup_result` ช่วงเวลาหนึ่ง) **การเพิ่ม** (`addFriend`, `line_friend_add()`) ทำได้เฉพาะกับผลลัพธ์ที่จำไว้นั้น และเฉพาะถ้า `canAdd` เป็นจริง มันเรียก `addFriendByMid` พร้อมลำดับคำขอ MID และ union *tracking* ที่บันทึกว่าพบคนนี้ได้อย่างไร (ค้นด้วย ID, ticket หรือเบอร์โทร) เหมือนที่แอป LINE ทำ

การเพิ่มคนจากที่อื่นใช้แขนอื่นของ union tracking ตัวเดียวกัน (`line_friend_add_from()`)

| แหล่ง | แขนของ tracking |
| ----- | --------------- |
| จากรายชื่อสมาชิกกลุ่ม | 5 |
| จากข้อความของคนแปลกหน้า | 13 |
| จากคำขอเป็นเพื่อน | 21 |
| จากคำแนะนำ (`addRecommendation`) | 10 พร้อมการอ้างอิง `{"screen":"friendAdd:recommend","spec":"native"}` |

**รูป QR** **Read QR image** ถอดรหัสรูปที่บันทึกไว้ด้วย ZBar ([`qr_scan.c`](../../src/qr_scan.c) ซึ่งโหลด `libzbar.so.0` ด้วย `dlopen` เฉพาะตอนใช้) แล้วส่งข้อความที่ได้เข้าการค้นหาด้วยลิงก์

**ลิงก์เชิญของคุณเอง** `friendInvite` เรียก `getUserTicket` บน `/S4` แล้วสร้างลิงก์ส่วนตัว `https://line.me/ti/p/<ticket>` ซึ่ง UI แสดงเป็น QR code

## การจัดการเพื่อน

`friend_action()` ใน `main.c` ตรวจว่าเพื่อนอยู่ในรายชื่อของ worker จองลำดับคำขอ แล้วเรียก `line_friend_rename()`, `line_friend_favorite()`, `line_friend_visibility()` หรือ `line_friend_block()` ทุกตัวใช้ `{1: {1: reqSeq, 2: mid, 3: ค่า}}` บน `/RE4`

| การกระทำ | เมธอด | ค่า |
| -------- | ----- | --- |
| ชื่อที่แสดงสำหรับเพื่อนคนนี้ | `updateUserFriendOverridenName` (สะกดตาม LINE) | ชื่อ หรือว่างเพื่อล้าง |
| รายการโปรด | `updateFriendFavorite` | bool |
| ซ่อน | `updateFriendHidden` | bool |
| บล็อก เลิกบล็อก | `blockRelation`, `unblockRelation` | ไม่มี |

การเลิกบล็อกจะเลิกซ่อนด้วย

## การ์ดโปรไฟล์

`line_friend_card()` คืนการ์ดของคนคนหนึ่ง จะเป็นเพื่อนหรือไม่ก็ได้: ชื่อ ชื่อที่คุณตั้งให้ รูป ข้อความสถานะ (`TargetProfileDetail` ฟิลด์ 4 ซึ่งฟิลด์ 1 ข้างในคือข้อความ ใน V3 และ `Contact` ฟิลด์ 26 ใน V2) และแฟล็กความสัมพันธ์ worker จำการ์ดที่เปิดไว้ (`remember_card()`) `openProfileChat`, `addProfileFriend` และ `setProfileBlocked` จึงทำกับคนที่ UI แสดงจริงเท่านั้น

## โปรไฟล์ของคุณเอง

- **อ่าน:** `myProfile` → `line_own_profile()` จาก `getProfile` บน `/S4` (MID ในฟิลด์ 1 ชื่อใน 20 สถานะใน 24 รูปใน 33)
- **แก้ไข:** `updateMyProfile` → `line_own_profile_update()`: `updateProfileAttributes` บน `/S4` ด้วย `{1: reqSeq, 2: {1: {<attribute>: {1: ค่า, 2: {}}}}}` โดย attribute 2 คือชื่อที่แสดง (ไม่เกิน 20 ตัวอักษร) และ 16 คือข้อความสถานะ (ไม่เกิน 500) ส่งเฉพาะ attribute ที่เปลี่ยน
- **รูป:** `updateMyPicture` ดู [มีเดีย](media.md)

## คำขอเป็นเพื่อน

ในที่ที่ LINE มีคำขอเป็นเพื่อน

- **ดูรายการ:** `getFriendRequests` บน `/S4` ด้วย `{1: 1 (INCOMING), 2: 0 (lastSeenSeqId)}` FriendRequest แต่ละตัวมี `eMid`, `mid` ทิศทาง วิธี param เวลา seqId ชื่อที่แสดง และพาธรูป
- **ยอมรับ:** เพิ่มคนนั้นด้วยแขน tracking ของคำขอเป็นเพื่อน (21)
- **ปฏิเสธ:** `removeFriendRequest` ด้วย `{1: 1, 2: eMid หรือ mid}`

คำขอใหม่มาสดเป็น OpType 88 และการเพิ่มใหม่เป็น OpType 5 (ดู [Sync](sync.md)) UI แสดง toast และนับไว้บน Inbox

## กลุ่ม

กลุ่มคือ *แชต* ใน TalkService ชนิด GROUP (0) ชนิด ROOM (1) แบบเก่าอ่านได้แต่ไม่ได้สร้าง worker ตรวจทุก MID ที่คำขอเกี่ยวกับกลุ่มระบุถึง เทียบกับรายชื่อเพื่อนของมัน และการเปลี่ยนแปลงแต่ละครั้งใช้ลำดับคำขอหนึ่งตัวก่อนการเขียนครั้งเดียวของมัน

### การอ่านข้อมูลกลุ่ม

`getChats` บน `/S4` ด้วย `{1: {1: [chat], 2: true, 3: true}, 2: 7}` คืน Chat

| ฟิลด์ | ความหมาย |
| ----- | -------- |
| 1 | ชนิด (0 กลุ่ม) |
| 2 | MID ของแชต |
| 6 | ชื่อ |
| 7 | พาธรูป |
| 8 → 1 | ข้อมูลเสริมของกลุ่ม: ฟิลด์ 2 "ปิดการเชิญด้วยลิงก์" ฟิลด์ 3 ticket สำหรับเชิญ ฟิลด์ 4 map ของ MID สมาชิก ฟิลด์ 5 map ของ MID ผู้ที่ถูกเชิญและยังรออยู่ |

`groupMembers` (`line_group_members()`) คืนสมาชิกและผู้ที่ถูกเชิญที่ยังรออยู่ พร้อมชื่อและรูป

### การเปลี่ยนแปลงกลุ่ม

| เมธอดของ worker | RPC บน `/S4` | อาร์กิวเมนต์ |
| --------------- | ------------ | ------------ |
| `createGroup` | `createChat` | `{1: {1: reqSeq, 2: 0 (GROUP), 3: ชื่อ, 4: set ของ MID เพื่อน}}` |
| `inviteGroup` | `inviteIntoChat` | `{1: {1: reqSeq, 2: chat, 3: set ของ MID}}` |
| `renameGroup` | `updateChat` | `{1: {1: reqSeq, 2: {2: chat, 6: ชื่อ}, 3: 1 (NAME)}}` |
| `setGroupLink` | `updateChat` | `{1: {1: reqSeq, 2: {2: chat, 8: {1: {2: !enabled}}}, 3: 4 (PREVENTED_JOIN_BY_TICKET)}}` |
| `renewGroupLink` | `reissueChatTicket` | `{1: {1: reqSeq, 2: chat}}` ฟิลด์ 1 ของคำตอบคือ ticket ใหม่ |
| `groupLink` | (อ่านอย่างเดียว) | สร้างลิงก์เชิญจาก ticket ในข้อมูลเสริมของกลุ่ม |
| `removeGroupMember` | `deleteOtherFromChat` | `{1: {1: reqSeq, 2: chat, 3: {สมาชิก}}}` |
| `cancelGroupInvite` | `cancelChatInvitation` | เหมือนกัน สำหรับผู้ที่ถูกเชิญและยังรออยู่ |
| `leaveGroup` | `deleteSelfFromChat` | `{1: {1: reqSeq, 2: chat}}` |

คุณต้องเป็นสมาชิกปัจจุบันสำหรับทุกการเปลี่ยนแปลง ยกเว้นการตอบคำเชิญ (`joined_group()` ตรวจ map ของสมาชิก) หลังออกจากกลุ่ม worker จำกลุ่มไว้ใน `left_groups` และ UI ลบกลุ่มออก (`ui_group_left()`)

### คำเชิญถึงคุณ

- **ดูรายการ:** `getAllChatMids` ด้วย `{1: {1: false, 2: true (แชตที่ถูกเชิญ)}, 2: 7}` แล้ว `getChats` เพื่อดึงชื่อ worker เก็บรายการไว้ใน `invited_groups` และรับคำตอบเฉพาะรหัสเหล่านั้น
- **เข้าร่วมหรือปฏิเสธ:** `acceptChatInvitation` หรือ `rejectChatInvitation` ด้วย `{1: {1: reqSeq, 2: chat}}`

## สิ่งที่ LINE ไม่มีให้ตรงนี้

API ของบัญชีส่วนตัวของ LINE ไม่มีสถานะออนไลน์ ไม่มีการแสดงว่ากำลังพิมพ์สำหรับไคลเอนต์เดสก์ท็อป ไม่มี "กลุ่มที่อยู่ร่วมกัน" ฝั่งเซิร์ฟเวอร์ และไม่มีแบนเนอร์โปรไฟล์ ไคลเอนต์จึงไม่แสดงสิ่งเหล่านี้ การลบเพื่อน การแก้รูปกลุ่ม และโหมดเพิ่มอัตโนมัติยังไม่ได้ทำ [analysis/desktop-features.md](../../analysis/desktop-features.md) ติดตามสิ่งที่ยังขาด
