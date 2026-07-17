# Dashboard UX and Bidirectional Navigation Design

## Goal

Refine the ESP32-2432S024C dashboard so every retained page communicates useful data, provider identity is clear, Activity uses semantic status colors, and touch navigation works in both directions.

## Page model

The dashboard has exactly three pages, in this order:

1. Claude
2. Codex
3. Activity

The Robot page that displays the mascot with `USB data · just now` or equivalent transport freshness text is removed from the dashboard carousel. The mascot may remain available to the existing boot, pairing, or idle flows; it is no longer a manually or automatically reachable dashboard page.

Automatic navigation continues to move forward every 12 seconds. A manual navigation event continues to defer automatic navigation for 30 seconds. The page indicator contains three dots and remains visible on all three dashboard pages.

## Provider identity

The shared header image becomes page-aware:

- Claude displays the existing Claude logo.
- Codex displays the official monochrome OpenAI Blossom, used as the compact Codex provider mark.
- Activity displays both provider marks: Claude on the left and OpenAI/Codex on the right, with `Activity` centered.

The OpenAI mark must preserve the official proportions and clear shape. It is stored as a small local firmware asset; the device does not fetch remote images. Both provider marks use comparable visual weight and must not overlap the title or screen edges in portrait or landscape layouts.

## Activity layout and colors

Activity retains one panel per provider. The Claude panel presents three independent rows instead of a single multiline label:

- `Open` and its value use green (`#55A868`).
- `Busy` and its value use orange (`#D97757`).
- `Waiting` and its value use blue (`#5B8FF9`).

The Codex panel presents `Unread` and its value in violet (`#8B7CF6`). Provider names remain in the normal foreground color. Unavailable states use the existing muted color and `Unavailable` text; no fabricated zero is shown.

The existing Activity freshness footer remains unchanged and continues to reflect only Activity updates.

## Touch navigation

Each completed short tap is classified by its logical X coordinate:

- `x < screen_width / 2`: navigate to the previous page.
- `x >= screen_width / 2`: navigate to the next page.

Navigation wraps in both directions:

- previous from Claude opens Activity;
- next from Activity opens Claude.

The split uses logical display coordinates after board-local touch transformation, so it behaves identically in portrait and landscape builds. Long-press and wake-consumption behavior remain unchanged. No visible arrow controls are added.

## Data and architecture

No daemon payload or data-collection contract changes are required. The implementation is limited to:

- carousel previous-page behavior;
- click-coordinate routing in the UI navigation layer;
- removal of the Robot page from the dashboard page enum and visibility flow;
- a local Codex/OpenAI image descriptor;
- Activity widgets and semantic styling;
- corresponding layout and contract tests.

The existing splash/mascot subsystem remains intact outside the dashboard carousel.

## Verification

Automated verification must cover:

- the three-page forward order and forward wrap;
- previous-page order and backward wrap;
- left-half versus right-half routing, including the center boundary;
- 12-second automatic advance and 30-second manual holdoff in either direction;
- absence of a Robot dashboard page and three-dot indicator geometry;
- page-aware provider-logo visibility;
- semantic Activity colors and unavailable states;
- portrait and 320x240 landscape layout bounds;
- both PlatformIO builds and the physical Unity suite.

Physical acceptance on the connected ESP32-2432S024C must confirm upright, unmirrored rendering, correct provider logos, readable Activity colors, left/right tap behavior, and normal serial updates after the production landscape firmware is restored.
