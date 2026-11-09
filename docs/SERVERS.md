# Jellyfin, Emby and Plex

This branch of WiiFin plays from three kinds of media servers. You do not choose
one: you type the server's address, and WiiFin works out which kind it is the
first time it talks to it.

| | Jellyfin | Emby | Plex |
|---|---|---|---|
| Find on the network (Discover) | yes | yes | yes (GDM) |
| Sign in | user + password, Quick Connect (code, QR) | user + password | see below |
| Home rows, libraries, sort, search | yes | yes | yes |
| Film / series / music pages | yes | yes | yes |
| Direct play | yes | yes | yes |
| Transcode | MPEG-4 + MP3 | MPEG-4 + MP3 | H.264 640×480 + MP3 |
| Text subtitles drawn by WiiFin | yes | yes | external `.srt` files |
| Other subtitles | burned in | burned in | burned in |
| Watched, resume points | yes | yes | yes (resume points: with an account) |
| Favourites | yes | yes | a rating of 10 stars |
| Skip intro / credits | media segments | – | Plex markers |
| Remote control from the server | yes | yes | – |
| Trickplay thumbnails, lyrics | yes | – | – |

## Signing in to Plex

* **No account**: on the server, *Settings > Network > List of IP addresses
  and networks that are allowed without auth*, add your home network
  (`192.168.1.0/24`...). Then leave the user name empty on the Wii.
* **Your Plex account**: type your plex.tv e-mail (or user name) and password.
* **A code**: the *Quick Connect* tab shows a code to type at
  [plex.tv/link](https://plex.tv/link) on a phone or a computer, and a QR
  code that opens Plex's sign-in page with it already filled in. (With
  Jellyfin, the same QR code opens the server's Quick Connect page.)
* **A token**: user name `token`, and the token as the password.

The server's address is its local one, such as `http://192.168.1.10:32400`.

## How it works

The interface of WiiFin still speaks Jellyfin: `source/jellyfin/JellyfinClient.h`
is unchanged for it. Behind it:

* `JellyfinApi.inc` is WiiFin's own Jellyfin client, used for Emby as well
  (Emby has an item only under its user, `/Users/{id}/Items/{id}`, and names a
  file `mediasource_<id>`).
* `PlexApi.inc` maps the same calls to Plex's API; the Jellyfin queries the
  interface builds (`/Users/<id>/Items?ParentId=...&SortBy=...`) are translated
  by `plexPathFor`. A library's id is `s<section>`, an item's its `ratingKey`.
* `Dispatch.inc` sends each call to the backend of its server
  (`backendFor`: Plex answers `/identity`, Jellyfin and Emby
  `/System/Info/Public`, and only Jellyfin names itself there).

Plex's transcoder has no MPEG-4 encoder, so a Plex transcode is H.264 Main,
level 3.0, at most 640×480, which MPlayer CE decodes on the Wii as it does
for direct play.

## Testing

`tools/test/multi/servers.sh` starts the three servers (podman or docker) on a
small library made by `make_media.sh` with any ffmpeg; `tools/test/plextour.sh`
runs WiiFin in Dolphin against one of them with scripted presses:

```sh
tools/test/multi/servers.sh
tools/test/plextour.sh "{ 10000, WPAD_BUTTON_A }, { 20000, WPAD_BUTTON_A }, { 30000, WPAD_BUTTON_A }, { 34000, WPAD_BUTTON_A }," "40 50" 170
SERVER=http://127.0.0.1:8096 SESSION="$(cat tools/test/multi/out/emby-session)" tools/test/plextour.sh ...
SERVER=http://127.0.0.1:18097 SESSION="$(cat tools/test/multi/out/jf-session)" tools/test/plextour.sh ...
```
