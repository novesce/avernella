# แอป LINE (LIFF) และเบราว์เซอร์ในแอป

[สารบัญ](README.md) · ก่อนหน้า: [เพื่อน โปรไฟล์ และกลุ่ม](social.md) · ถัดไป: [การโทร](calls.md)

**แอป LIFF** (LINE Front-end Framework) คือหน้าเว็บที่ทำงาน *ภายใน* LINE พร้อมตัวตน LINE ของผู้ใช้ เช่น คูปอง บัตรสมาชิก การจอง และมินิแอป บัญชีทางการลิงก์ไปหาแอปเหล่านี้จากการ์ด Flex และ rich message หลายแอปไม่ยอมทำงานนอก LINE ("ลิงก์นี้ใช้ได้เฉพาะใน LINE") Avernella เปิดแอปเหล่านี้แบบเดียวกับแอป LINE บนโทรศัพท์: ขอ *view* จาก LINE ส่งโทเค็นของ LINE ให้หน้าเว็บ แล้วรันในหน้าต่างเบราว์เซอร์ที่หน้าเว็บมองว่าเป็น LINE

ตารางฟิลด์ฉบับเต็มจากแอป Android และ LIFF SDK อยู่ใน [analysis/liff.md](../../analysis/liff.md)

## ส่วนประกอบ

| ส่วน | โปรเซส | ไฟล์ |
| ---- | ------ | ---- |
| จดจำลิงก์ LIFF, user agent, โปรโตคอลกับเบราว์เซอร์ | ทั้งสอง | [`liff_url.c`](../../src/liff_url.c), [`liff_url.h`](../../src/liff_url.h) |
| ขอ view จาก LINE และสร้างที่อยู่หน้าเว็บ | worker | [`liff.c`](../../src/liff.c) |
| ตัดสินว่าลิงก์จะเปิดที่ไหน เริ่มและดูแลเบราว์เซอร์ | UI | [`ui_liff.c`](../../src/ui_liff.c) |
| หน้าต่างเบราว์เซอร์ native bridge, sandbox และ cgroup | `avernella-liff` | [`liff_browser.c`](../../src/liff_browser.c) |

## การเปิดลิงก์

`ui_liff_open_link(url)` ถูกเรียกทุกครั้งที่ผู้ใช้กดลิงก์ในการ์ด Flex หรือ rich message

1. **ลิงก์ LIFF**: `line_liff_url()` รู้จัก `https://liff.line.me/<liffId>…`, `https://miniapp.line.me/<liffId>…`, `https://line.me/R/app/<liffId>…` และ `line://app/<liffId>…` LIFF ID คือ channel ID ขีดกลาง และส่วนท้าย เช่น `1661444788-rnvmoOo8` ทุกอย่างที่ตามหลัง (path, query, fragment) คือ *deep link* UI ส่ง `liffView {url, chat}` ไปที่ worker และคำตอบกลายเป็นข้อความ `L` ถึงเบราว์เซอร์
2. **ลิงก์ HTTPS อื่น ๆ** ส่งไปที่เบราว์เซอร์เป็นข้อความ `W` และเปิดด้วย user agent ในแอปของ LINE
3. **ไม่ได้ติดตั้งโปรแกรมเบราว์เซอร์:** ลิงก์เปิดในเบราว์เซอร์ของระบบ (`SDL_OpenURL()`)

## การขอ view จาก LINE

`line_liff_view()` ใน worker เรียก `issueLiffView` บน `/LIFF1`

```
issueLiffView({1: LiffViewRequest {
    1: liffId,
    2: context  — {2: {1: MID ของแชต}} เมื่อเปิดจากแชต ไม่เช่นนั้น {1: {}} (none)
    3: lang     — locale ของเดสก์ท็อป เช่น "en_US" หรือ "th_TH" (`liff_lang()`)
}})
```

คำตอบ (`LiffViewResponse`) ให้

| ฟิลด์ | ความหมาย |
| ----- | -------- |
| 1 | view: ฟิลด์ 1 คือขนาด (`FULL`, `TALL` หรือ `COMPACT`) ฟิลด์ 2 คือ URL ของ endpoint ฟิลด์ 22 คือชื่อแอป |
| 2 | context token |
| 3 | access token สำหรับแอป |
| 4 | feature token |
| 6 | channel ID |
| 7 | ID token |

**การสร้างที่อยู่หน้าเว็บ** `line_liff_launch_url()` เริ่มจาก endpoint (HTTPS เท่านั้น) ต่อ path ของ deep link ไว้ใต้ path ของ endpoint ต่อ query parameter และใส่โทเค็นไว้ใน **fragment**

```
https://app.example/path/deep?x=1#context_token=…&feature_token=…&access_token=…&id_token=…&client_id=…
```

`liff.init()` ของ LIFF SDK อ่านโทเค็นจาก `location.hash` เก็บไว้ใน `sessionStorage` แล้วลบออกจากที่อยู่ fragment ไม่เคยถูกส่งไปยังเว็บเซิร์ฟเวอร์ โทเค็นจึงถึงแค่ JavaScript ของหน้านั้นเอง

**เมื่อ LINE ปฏิเสธ** `LiffException` (รหัสในฟิลด์ 1 และ union ของ payload ในฟิลด์ 3) จัดการใน `exception_page()`

| รหัส | ความหมาย | ไคลเอนต์ทำอะไร |
| ---- | -------- | -------------- |
| 3 CONSENT_REQUIRED | ผู้ใช้ต้องยอมรับสิทธิ์ของแอปก่อน | เปิดหน้าขอความยินยอม (payload 3 ฟิลด์ 2) ในหน้าต่างเดิม |
| 7 PERMANENT_LINK_INVALID_REQUEST | deep link ใช้ไม่ได้แล้ว | เปิดหน้าสำรอง (payload 4 ฟิลด์ 2) |
| อื่น ๆ | ไม่มีสิทธิ์ ต้องอัปเดต บริการยุติแล้ว ฯลฯ | แสดงเหตุผลที่อ่านเข้าใจ (`refusal()`) |

worker ตอบกลับเป็น `{url, size, title}`

## โปรแกรมเบราว์เซอร์

`avernella-liff` เป็นโปรแกรมแยก build ด้วย **WPE WebKit** เมื่อมี (เลือกใช้บน Wayland) และเป็น `avernella-liff-gtk` ด้วย **WebKitGTK 6.0** เป็นตัวสำรอง (และสำหรับ X11) ทั้งสองมาจาก `liff_browser.c` ไฟล์เดียว `LINE_LIFF_GTK` เลือก build แบบ GTK

### การเริ่มและหยุด

`ui_liff.c`

- หาโปรแกรมข้าง `avernella` หรือใน `../lib/avernella` เลือก WPE เมื่อมีจอ Wayland
- เริ่มเมื่อมีลิงก์ **แรก** ด้วย `pidfd_spawn()` ส่งซ็อกเก็ต `SOCK_SEQPACKET` เป็น fd 3 และ PID ของโปรเซสแม่เป็นอาร์กิวเมนต์เดียว
- ดูแลผ่าน pidfd และรันเธรดอ่านที่หลับใน `recv()` และส่ง SDL event เฉพาะเมื่อเบราว์เซอร์ส่งอะไรมา
- ถือว่า end of file บนซ็อกเก็ตคือ "เบราว์เซอร์ปิดแล้ว" และเก็บกวาดโปรเซส

เบราว์เซอร์ปิดเมื่อหน้าต่างของมันปิด จึงไม่เปลืองอะไรตอนไม่ได้ใช้ การออกจากระบบและการปิดโปรแกรมจะปิดมัน (`ui_liff_close()`)

### โปรโตคอลบนซ็อกเก็ต

หนึ่ง datagram ต่อหนึ่งข้อความ ฟิลด์คั่นด้วยแท็บ ยาวไม่เกิน 16 KiB (`LINE_LIFF_PACKET`)

| ทิศทาง | ข้อความ | ความหมาย |
| ------ | ------- | -------- |
| UI → เบราว์เซอร์ | `L\t<size>\t<url>` | โหลดหน้า LIFF: `full`, `tall` หรือ `compact` ด้วย user agent ของ LIFF |
| UI → เบราว์เซอร์ | `W\tweb\t<url>` | โหลดหน้าเว็บธรรมดาด้วย user agent ในแอปของ LINE |
| UI → เบราว์เซอร์ | `R\t<dialog id>\t<1\|0>` | คำตอบของกล่องโต้ตอบ JavaScript |
| เบราว์เซอร์ → UI | `O\t<url>` | หน้าเว็บไปที่ลิงก์ LIFF หรือ `line://`: ขอ view ใหม่จาก LINE |
| เบราว์เซอร์ → UI | `X\t<url>` | เปิดลิงก์นี้บนเดสก์ท็อป (เบราว์เซอร์ของระบบ อีเมล โทรศัพท์) |
| เบราว์เซอร์ → UI | `D\t<dialog id>\t<a\|c>\t<text>` | แสดง `alert()` (`a`) หรือ `confirm()` (`c`) ในหน้าต่างของไคลเอนต์ |

โทเค็นเดินทางผ่านซ็อกเก็ตนี้เท่านั้น ไม่ผ่านอาร์กิวเมนต์หรือตัวแปรสภาพแวดล้อม และไม่เคยถูกบันทึกลง log

### ทำให้หน้าเว็บมองว่าเป็น LINE

- **User agent** หน้า LIFF ได้ `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Line/9.7.0 LIFF` (`LINE_LIFF_USER_AGENT`) `liff.isInClient()` เป็นจริงเมื่อ user agent ตรงกับ `Line/x.y.z LIFF` หน้าธรรมดาได้แบบเดียวกันแต่ไม่มี ` LIFF` (`LINE_WEB_USER_AGENT`)
- **Native bridge** ถ้า user agent มีเวอร์ชัน LINE 9.5 ขึ้นไป `liff.init()` จะรอ `window._liff.features` ก่อนจะ resolve สคริปต์ที่ฉีดตอนเริ่มเอกสารนิยาม `window._liff = { features: [], postMessage }` แบบ freeze ไว้ `postMessage(action, featureToken, callbackId, json)` ไปถึงเบราว์เซอร์ผ่าน script message handler ของ WebKit
  - `closeWindow` ปิดหน้าต่าง
  - `openWindow` โหลด `url` ของมัน หรือส่งให้เดสก์ท็อปเมื่อ `external` เป็นจริง
  - อย่างอื่น (สแกน QR ตัวเลือกการแชร์ และฟีเจอร์ที่มีเฉพาะบนโทรศัพท์) ตอบทันทีด้วย `liffEvent` ที่มี `detail` เป็น `{type, callbackId, error: {code: "FORBIDDEN"}}` การเรียกของ SDK จึงถูกปฏิเสธแทนที่จะค้าง
- **การปิด** ไปที่ `liff://close` หรือเรียก `window.close()` จะปิดหน้าต่าง
- **ลิงก์ของ LINE** ในหน้า (`line_liff_url()` หรือ `line_liff_line_scheme()`) ถูกส่งกลับไปที่ UI เป็น `O` แต่ละลิงก์จึงได้ view ของตัวเองจาก LINE
- **สิทธิ์** คำขอใช้กล้อง ไมโครโฟน ตำแหน่ง และการแจ้งเตือนถูกปฏิเสธ
- **HTTPS เท่านั้น** การนำทางทุกครั้ง ไม่ว่าจะเป็นลิงก์ สคริปต์ หรือการ redirect จากเซิร์ฟเวอร์ ต้องอยู่บน `https://` หรือบนเอกสาร `about:`, `data:` และ `blob:` ที่หน้าเว็บสร้างเอง (`line_liff_web_url()`) ลิงก์ `http://` ธรรมดาที่ผู้ใช้คลิกจะเปิดในเบราว์เซอร์ของเดสก์ท็อปแทน และการย้ายไปยังลิงก์แบบนี้ในทางอื่นจะถูกทิ้ง ไม่มีใครบนเครือข่ายจึงแทนที่หน้าเว็บที่ถือโทเค็นของ LINE ได้

### ปุ่มในหน้าต่างเบราว์เซอร์

Ctrl+Shift+O เปิดหน้าปัจจุบันในเบราว์เซอร์ของคุณเอง (ไม่มีโทเค็นของ LINE) Alt+← และ Alt+→ ย้อนกลับและไปข้างหน้า Ctrl+R โหลดใหม่ Ctrl+บวก Ctrl+ลบ และ Ctrl+0 ซูม และ Ctrl+W ปิด

### การจำกัดทรัพยากร

เบราว์เซอร์ปกป้องแชตจากหน้าเว็บที่หนัก

- ย้ายตัวเองและทุกโปรเซสของ WebKit เข้า **systemd transient scope** ซึ่งเป็น cgroup v2 แยกของตัวเอง ที่มี `MemoryHigh=768M`, `CPUWeight=50` และ `IOWeight=50` ผ่าน `org.freedesktop.systemd1.Manager` และรอจนกว่า `/proc/self/cgroup` จะแสดงว่าย้ายแล้วก่อนที่ WebKit จะ fork โปรเซสลูกจึงอยู่ใน scope ด้วย
- เพิ่ม `oom_score_adj` เป็น 500 เคอร์เนลจึงเลือกเก็บคืนหน่วยความจำจากมันก่อนไคลเอนต์ และตั้ง `PR_SET_PDEATHSIG` ให้จบตามไคลเอนต์
- ตั้ง timer slack 1 ms (`PR_SET_TIMERSLACK`) ซึ่งโปรเซสของ WebKit สืบทอดไป ตัวจับเวลาของหน้าเว็บจึงรวมกันเป็นการตื่นที่น้อยลง
- web process ของ WebKit ถูกจำกัดที่ 512 MB และ network process ตัดแคชเมื่อเกิน 128 MB ใช้แคชหน่วยความจำขนาดเล็กและไม่มีแคชหน้าสำหรับย้อนกลับ วิดีโอรอให้คลิกก่อนเล่น เมื่อ WebKit หยุดหน้าเว็บเพราะใช้หน่วยความจำมากเกินไป เบราว์เซอร์จะโหลดหน้านั้นใหม่หนึ่งครั้งหลังผ่านไป 3 วินาที ถ้าเกิดซ้ำ หน้าต่างจะปิดแทนการโหลดหน้าเดิมซ้ำไม่รู้จบ
- sandbox แบบ bubblewrap ของ WebKit ยังเปิดอยู่
- แคช HTTP และคุกกี้อยู่ใน `~/.cache/avernella/liff` และ `~/.local/share/avernella/liff` ถ้ายังไม่มีไดเรกทอรีเหล่านี้ `line_liff_storage_dir()` ใน `liff_url.c` จะย้ายไดเรกทอรีเดียวกันของไคลเอนต์รุ่นก่อน (`kitatide` แล้วจึง `line-linux`) เข้ามา แอป LINE จึงยังมีคุกกี้เดิม ไดเรกทอรีนี้ต้องเป็นไดเรกทอรีจริงที่คุณเป็นเจ้าของ อยู่ในไดเรกทอรีแม่ที่ผู้ใช้อื่นเขียนไม่ได้ และถูกตั้งเป็นโหมด 0700 symlink ไฟล์ หรือไดเรกทอรีของผู้ใช้อื่นจะถูกปฏิเสธ แล้วเบราว์เซอร์จะไม่เก็บอะไรลงดิสก์

## การดีบักแอป LINE

รันด้วย `LINE_DEBUG_LIFF=1` แล้วเปิดแอปอีกครั้ง เทอร์มินัลจะแสดงขั้นตอนการโหลดของ WebKit และคำสั่ง bridge เป็น `openWindow`, `closeWindow` หรือ `unsupported` การใช้ป้ายกำกับคงที่ช่วยป้องกันไม่ให้ URL ข้อความจาก console ของหน้าเว็บ และโทเค็นปรากฏในบันทึกการดีบัก LIFF ของ Avernella ตัวแปรเดียวกันนี้ยังทำให้เบราว์เซอร์โหลดหน้า `http://127.0.0.1:<port>/` ได้ (`line_liff_debug_url()`: พอร์ต 1024 ขึ้นไป และไม่มีอะไรอื่นก่อน path เช่นข้อมูลผู้ใช้) หน้าทดสอบในเครื่องที่โหลด SDK ตัวจริง (`https://static.line-scdn.net/liff/edge/2/sdk.js`) จึงทดสอบ bridge ได้ ดู [การ build การรัน และการดีบัก](building.md)

## เทสต์

`tests/liff.c` ตรวจคำขอ `issueLiffView` ทีละฟิลด์ การสร้าง URL สำหรับเปิด เส้นทางขอความยินยอมและหน้าสำรอง และการจดจำลิงก์ โดยใช้ fixture ของ RPC ที่อธิบายใน [การทดสอบ](testing.md)

รัน `python3 tests/liff_browser_debug.py ./avernella-liff` เพื่อตรวจบันทึกการดีบักด้วยหน้าเว็บในเครื่องและโทเค็นจำลอง สำหรับ helper ของ GTK ใช้จอเสมือนด้วยคำสั่ง `xvfb-run -a python3 tests/liff_browser_debug.py ./avernella-liff-gtk`
