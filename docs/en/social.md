# Friends, profiles and groups

[Index](README.md) · Previous: [Media](media.md) · Next: [LIFF](liff.md)

The people side of LINE: finding and adding friends, managing them, profiles, friend requests, and groups. Friends and profiles are in [`friends.c`](../../src/friends.c), groups in [`groups.c`](../../src/groups.c), and the worker glue in [`main.c`](../../src/main.c) (`friend_directory()`, `find_friend()`, `add_friend()`, `friend_action()`, `profile_card()`, `profile_action()`, `friend_requests()`, `answer_request()`, `change_group()`, `pending_groups()`, `answer_group()`).

## Two services

Most friend operations go to **RelationService** on `/RE4`. Profile, group and request operations go to **TalkService** on `/S4`.

RelationService failures carry their own error enum. `relation()` in `friends.c` turns the common codes into readable messages and then resets `s->service_code`, so they aren't mistaken for TalkService codes:

| Code | Message shown |
| ---- | ------------- |
| 1 | LINE could not find an available account for this request. |
| 2 | LINE requires age verification for this account to use friend search. |
| 3 | LINE's friend limit has been reached. |
| 4 | LINE temporarily limited friend requests. Try again later. |
| 5 | LINE declined the friend request data. |
| 6 | LINE requires this person's QR invitation for this request. |

## The friend directory

`line_friends_page()` and `line_managed_friends_page()`:

1. **IDs:** `getUserFriendIds` on `/RE4` with `{1: {1: cursor, 2: 2}}` (2 = not blocked). The blocked view uses 1, and the recommendations view uses `getUserRecommendIds`. Reply field 1 is a page of MIDs and field 2 the next cursor. The worker owns the cursor; the UI only says "next page".
2. **Details:** `getContactsV3` on `/RE4` with `{1: {1: [{1: mid}, …]}}`, falling back to `getContactsV2` on `/S4` for older servers.
3. **Rows:** `{id, profileName, overrideName, avatarKey, favorite, hidden, blocked, canAdd, official, recommended}`. Hidden and blocked friends are left out of the main directory and shown under **Manage**.

## Finding and adding a friend

**Lookup** (`findFriend`, `line_friend_lookup()`) accepts three kinds of input, and `search_value()` checks each strictly:

| Kind | Example | RPC on `/RE4` |
| ---- | ------- | ------------- |
| LINE ID | `alice.1` | `findContactBySearchIdOrTicketV3` with `{1: {1: {1: "alice.1"}}}` |
| Personal link or ticket | `https://line.me/ti/p/AbC_1-xy`, `line://ti/p/…` | the same method with the ticket in field 2: `{1: {1: {2: ticket}}}`. A `~id` link is treated as a LINE ID. |
| Phone | `+12025550123` (international format) | `findContactByPhoneV3` with `{1: {1: "+12025550123"}}` |

The result is remembered by the worker (`lookup_result`, for a limited time). **Adding** (`addFriend`, `line_friend_add()`) is allowed only for that remembered result, and only if `canAdd` was true. It calls `addFriendByMid` with the request sequence, the MID and a *tracking* union that records how the person was found (search by ID, ticket or phone), as LINE's app does.

Adding someone from elsewhere uses other arms of the same tracking union (`line_friend_add_from()`):

| Source | Tracking arm |
| ------ | ------------ |
| From a group's member list | 5 |
| From a stranger's message | 13 |
| From a friend request | 21 |
| From recommendations (`addRecommendation`) | 10, with a `{"screen":"friendAdd:recommend","spec":"native"}` reference |

**QR images.** **Read QR image** decodes a saved picture with ZBar ([`qr_scan.c`](../../src/qr_scan.c), loading `libzbar.so.0` with `dlopen` only when used) and feeds the text into the link lookup.

**Your own invite.** `friendInvite` calls `getUserTicket` on `/S4` and builds your personal `https://line.me/ti/p/<ticket>` link, which the UI shows as a QR code.

## Pacing searches and adds

Searching many LINE IDs or phone numbers in a short time, or adding many people at once, is what LINE treats as account enumeration or spam, and it is the most likely reason for LINE to restrict an account. The worker therefore paces these actions itself (`line_pace_wait()` and `line_pace_record()` in `friends.c`, applied by `paced()` in `main.c`):

| Action | Rule (`line_lookup_pace`, `line_add_pace`) |
| ------ | ------------------------------------------- |
| `findFriend` (LINE ID, link or phone lookup) | At least 3 seconds apart, and at most 10 in any 10 minutes |
| `addFriend`, `addRecommendation`, `addProfileFriend` | At most 10 in any 10 minutes. Add right after a search is fine. |
| Either, after LINE answers RelationService reason 4 ("temporarily limited") | Nothing more for an hour. `relation()` returns that reason as `LINE_RATE_LIMITED`. |

A refused action tells the user how long to wait ("To keep your account safe, wait 3 minutes before trying again."). The limits are well above what a person does by hand, so ordinary use never meets them. They reset when the app restarts.

## Managing friends

`friend_action()` in `main.c` checks that the friend is in the worker's roster, reserves a sequence, and calls `line_friend_rename()`, `line_friend_favorite()`, `line_friend_visibility()` or `line_friend_block()`. All take `{1: {1: reqSeq, 2: mid, 3: value}}` on `/RE4`:

| Action | Method | Value |
| ------ | ------ | ----- |
| Display name for this friend | `updateUserFriendOverridenName` (LINE's spelling) | the name, or empty to clear |
| Favorite | `updateFriendFavorite` | bool |
| Hide | `updateFriendHidden` | bool |
| Block, unblock | `blockRelation`, `unblockRelation` | none |
| Delete | `deleteFriend`, `deleteFromBlockList` | none (confirmed in UI; blocked friends use `deleteFromBlockList`) |

Unblocking also unhides. **Keep Memo** (`openKeepMemo`) opens your own `s->mid` chat directly from the Friends bar.

## Server-side account settings

`accountSettings` (`line_account_settings()`) calls `getSettings` (`2:i32 8`) on `/S4`, and `updateAccountSetting` (`line_account_settings_update()`) calls `updateSettingsAttributes2` (`1:i32 reqSeq`, `3:obj settings`, `4:set:i32 attributesToUpdate`) on `/S4`. Supported boolean fields: notification enable (`10`), message preview (`14`), mention notifications (`68`), reaction notifications (`95`), phone search (`21`), LINE ID search (`22`), secondary device login (`24`), non-friend messages (`26`), and Letter Sealing (`61`). These appear in **User Settings → Privacy & Safety** and **Notifications**.

## Profile cards

`line_friend_card()` returns one person's card, friend or not: the name, the name you gave them, the picture, the status message (`TargetProfileDetail` field 4, whose field 1 is the text, in V3; `Contact` field 26 in V2) and the relationship flags. The worker remembers opened cards (`remember_card()`), so `openProfileChat`, `addProfileFriend` and `setProfileBlocked` act only on a person the UI actually showed.

## Your own profile

- **Read:** `myProfile` → `line_own_profile()`, from `getProfile` on `/S4` (MID in field 1, name in 20, status in 24, picture in 33).
- **Edit:** `updateMyProfile` → `line_own_profile_update()`: `updateProfileAttributes` on `/S4` with `{1: reqSeq, 2: {1: {<attribute>: {1: value, 2: {}}}}}`, where attribute 2 is the display name (up to 20 characters) and 16 the status message (up to 500). Only changed attributes are sent.
- **Picture:** `updateMyPicture`, see [Media: profile pictures](media.md#profile-pictures).

## Friend requests

Where LINE offers friend requests:

- **List:** `getFriendRequests` on `/S4` with `{1: 1 (INCOMING), 2: 0 (lastSeenSeqId)}`. Each FriendRequest carries `eMid`, `mid`, direction, method, param, timestamp, seqId, display name and picture path.
- **Accept:** adds the person with the friend-request tracking arm (21).
- **Decline:** `removeFriendRequest` with `{1: 1, 2: eMid or mid}`.

New requests arrive live as OpType 88 and new adds as OpType 5 (see [Sync](sync.md#friend-and-profile-operations)). The UI shows a toast and counts them on the Inbox.

## Groups

Groups are TalkService *chats* of type GROUP (0). The old type ROOM (1) can be read but isn't created. The worker validates every MID a group request names against its friend roster, and each mutation consumes one request sequence just before its only write.

### Reading a group

`getChats` on `/S4` with `{1: {1: [chat], 2: true, 3: true}, 2: 7}` returns the Chat:

| Field | Meaning |
| ----- | ------- |
| 1 | Type (0 group) |
| 2 | Chat MID |
| 6 | Name |
| 7 | Picture path |
| 8 → 1 | Group extra: field 2 "invitation by link disabled", field 3 the invitation ticket, field 4 a map of member MIDs, field 5 a map of pending invitee MIDs |

`groupMembers` (`line_group_members()`) returns the members and pending invitees with their names and pictures.

### Changing a group

| Worker method | RPC on `/S4` | Arguments |
| ------------- | ------------ | --------- |
| `createGroup` | `createChat` | `{1: {1: reqSeq, 2: 0 (GROUP), 3: name, 4: set of friend MIDs}}` |
| `inviteGroup` | `inviteIntoChat` | `{1: {1: reqSeq, 2: chat, 3: set of MIDs}}` |
| `renameGroup` | `updateChat` | `{1: {1: reqSeq, 2: {2: chat, 6: name}, 3: 1 (NAME)}}` |
| `updateGroupPicture` | OBS `/r/talk/g/<chat>` | 640×640 JPEG uploaded via `line_group_picture_update()` |
| `setGroupLink` | `updateChat` | `{1: {1: reqSeq, 2: {2: chat, 8: {1: {2: !enabled}}}, 3: 4 (PREVENTED_JOIN_BY_TICKET)}}` |
| `renewGroupLink` | `reissueChatTicket` | `{1: {1: reqSeq, 2: chat}}`. Reply field 1 is the new ticket. |
| `groupLink` | (read only) | Builds the invitation link from the ticket in the group extra |
| `removeGroupMember` | `deleteOtherFromChat` | `{1: {1: reqSeq, 2: chat, 3: {member}}}` |
| `cancelGroupInvite` | `cancelChatInvitation` | the same, for a pending invitee |
| `leaveGroup` | `deleteSelfFromChat` | `{1: {1: reqSeq, 2: chat}}` |

You must be a current member for every change except answering an invitation (`joined_group()` checks the member map). After leaving, the worker remembers the group in `left_groups` and the UI drops it (`ui_group_left()`).

### Invitations to you

- **List:** `getAllChatMids` with `{1: {1: false, 2: true (invited chats)}, 2: 7}`, then `getChats` for names. The worker keeps the list in `invited_groups` and accepts answers only for those IDs.
- **Join or decline:** `acceptChatInvitation` or `rejectChatInvitation` with `{1: {1: reqSeq, 2: chat}}`.

## What LINE doesn't offer here

LINE's personal-account API has no presence, no typing indicators for desktop clients, no server-side "mutual groups" and no profile banners, so the client doesn't show them. Mobile-only and financial LINE Android services (LINE Pay, Wallet, Points, Coins, BLE Beacon, SIM registration, Google Drive backup, VOOM) are out of scope. [analysis/desktop-features.md](../../analysis/desktop-features.md) tracks remaining gaps.

## OpenChat and LINE links

OpenChat uses `SquareService` on `/SQ1` (`src/square.c`, `src/ui_openchat.c`) with distinct 33-character MID prefixes: `s` for a Square community, `m` for a SquareChat room, and `p` for a per-OpenChat member identity. OpenChat messages and `/r/g2/m/<id>` media attachments are plaintext (not Letter-Sealed), and OpenChat/member avatars load from `profile.line-scdn.net/<obsHash>`.

From the **OpenChat** (`#`) entry in the navigation bar or sidebar, users can browse joined OpenChats, discover public OpenChats via popular keywords (`getPopularKeywords`) and search (`searchSquares`), join by `line://ti/g2/…` or `https://line.me/ti/g2/…` link (open, passcode, or admin-approval), or create a new OpenChat. Inside an OpenChat room, typing `@` queries `searchSquareChatMentionables` for inline mention autocomplete, and the header **Member List** icon opens OpenChat info, per-OpenChat profile and photo editing, invite link and QR code, sub-chat rooms, settings, join-request approval, and member moderation (co-admin, kick, ban, and admin message deletion). Live updates poll `fetchMyEvents`.

Chat messages highlight and open clickable `https://`, `http://`, `line://`, and `lineb://` links, and `data/avernella.desktop` registers `x-scheme-handler/line` and `x-scheme-handler/lineb` so desktop links forward to the running window over the single-instance socket (`src/ui_instance.c`) and route OpenChat (`/ti/g2/`), group (`/ti/g/` via `findChatByTicket` / `acceptChatInvitationByTicket`), friend (`/ti/p/`), chat (`chatId=`), and LIFF (`/app/`) links in-app. Full protocol notes live in [analysis/openchat.md](../../analysis/openchat.md).
