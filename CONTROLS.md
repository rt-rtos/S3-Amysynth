# Controls

User-facing control scheme: which physical input does what, on each screen,
overlay and editor. The gesture dispatcher is `dispatch_button_event()` and
`encoder_process_steps()` in `main/main.c`; per-screen input handlers are in
`components/synth_core/synth_ui/`. When code and this file disagree, the code
is right and this file is stale.

## Inputs

| Name | Enum (`my_button_id_t`) | Notes |
|---|---|---|
| Encoder turn | - | 2 raw quadrature ticks = 1 step. Polled at 50 Hz |
| Encoder click | `MY_BUTTON_ENC` | Acts on press. There is no encoder long-press gesture |
| Button 0 | `MY_BUTTON_0` | Tap = `SINGLE_CLICK`, hold = `LONG_PRESS_START` |
| Button 1 | `MY_BUTTON_1` | Hold modifier (acts on press, released on release), or a press action in some views |
| Button 2 | `MY_BUTTON_2` | Same as button 1 |
| Button 3 | `MY_BUTTON_3` | Acts on tap |
| SHOULDER (LB) | `MY_BUTTON_SHOULDER` | Left shoulder button. Acts on press, except the LFO tab flip and the FM page flip (tap) |
| SHIFT (RB) | `MY_BUTTON_SHIFT` | Right shoulder button. Hold modifier; a bare tap does nothing |

Pin assignments: `components/my_buttons/my_buttons.c` (`s_button_gpios[]`).

## Screen and overlay precedence

Highest first. The first six are overlays that capture the encoder and all
button input; the rest are mode screens. Resolved in one place,
`synth_ui_active_view()` (`components/synth_core/include/synth_ui.h`).

1. Filter editor
2. LFO editor
3. Distortion (DIST) editor
4. Step Trig popup
5. ADSR (graph) editor
6. Menu
7. Mode screen: Arp, Drone visualiser, Stutter drone, Normal drone, Prog, FM, DEV, Seq (default at boot)

Screens are changed only through the Menu.

## Global gestures

| Gesture | Action |
|---|---|
| SHIFT + Button 0 (press) | Loop bounce: start into the first empty clip slot, cancel one still waiting for its bar line, or stop a running one at the next pattern-period boundary. Works from any screen |
| SHIFT + Button 0 (hold) | Discard a running bounce |
| SHIFT + Button 1 | Open the ADSR editor on the current instrument (Seq, Arp, Drone screens, or the Wireless menu page). With an editor open: commit and close it |
| SHIFT + Button 2 | Sequencer screen, no menu: open the Step Trig popup. Popup open: close it. Filter/LFO/DIST/ADSR editor open: release the open tab to the patch |
| SHIFT + Button 3 | Sequencer screen, no menu: open the menu on the Layer page, on the row it was left on if the menu was closed on that page, else on Gate. Menu open on the Layer page: close it. Filter/LFO/DIST/ADSR editor open on a melodic row: flip the row between its own voice block and the layer's shared block |
| Button 3 (tap) | Toggle the menu; it reopens on the page and row it was closed on. With an editor open: next editor. With the Step Trig popup open: close it |

A SHIFT chord is latched on its digit's press and swallows the rest of that
press, so the button's own tap/hold gesture never fires. A chord whose
preconditions are not met does nothing.

## Hint strip

The bottom line shows `1:<b1> 2:<b2> 3:<b3>` and, in the menu on a sub-page,
` LB:Main`; on the FM screen ` LB:Pg2` or ` LB:Pg1` (the page SHOULDER flips
to). It is read from `ui_view_table[]` (`ui_view_resolve.c`) for the
active view. SHIFT chords are not shown. The strip is hidden on the Prog
screen and on the drone visualiser; a DEV status bar replaces it on every screen
while on.

| View | 1 | 2 | 3 |
|---|---|---|---|
| Seq | Patch | Pitch | Menu |
| Arp, Stutter, Normal drone | Patch | - | Menu |
| FM | Link (page 1), - (page 2) | Mute | Menu (Done while linking) |
| Menu | - (Save while naming) | - (Disc while naming) | Menu |
| Prog | Del | +Add | Menu |
| DEV | - | - | Menu |
| ADSR editor | Type | Amp / Swg / Pit / Cut / Drv / Mix / Off (the next stop) | Next |
| Filter editor | On/Off | - | Next |
| LFO, DIST editor | - | - | Next |
| Step Trig popup | - | - | Close |

## Sequencer screen (Seq)

| Input | Action |
|---|---|
| Encoder turn | Move the step cursor. Off either end wraps to the adjacent track |
| Encoder click | Toggle the step under the cursor |
| SHOULDER (press) | Toggle the step under the cursor (second hand for step entry) |
| Button 0 (tap) | Cycle the active layer (resets the cursor to track 0, step 0) |
| Button 0 (hold) | Play / stop |
| Button 1 hold + turn | Cycle the patch of the selected track (drum layer: that drum track's patch) |
| Button 2 hold + turn | Transpose the selected track. Melodic: step through C1..C7 then the defined chord presets. Drum: semitones, clamped 0..127 |
| SHIFT + turn | Step the active melodic layer's FM algorithm live; a non-FM patch shows a NOT FM banner; drum layers ignore it. Button 1 hold or Button 2 hold take precedence |
| Button 3 (tap) | Menu |

## Step Trig popup

Addressed by the grid cursor (layer, track, step). Eight fields in a
five-row window that scrolls with the cursor.

| Input | Action |
|---|---|
| Encoder turn, navigating | Move to the next / previous field (wraps) |
| Encoder click | On Prev: toggle it. On any other field: enter or leave adjust mode |
| Encoder turn, adjusting | Change the value |
| Button 0 (tap or hold), Button 3 (tap), SHIFT + Button 2 | Close (there is no discard) |

| Field | Step per detent | Range |
|---|---|---|
| Pitch | 1 semitone | +/- `SEQ_STEP_PITCH_OFS_MAX` |
| Prob | 5 % | 0..100 |
| Ratchet | 1 | 1..`SEQ_MAX_RATCHET` |
| Every | 1 | 1..`SEQ_STEP_EVERY_MAX`; 1 = every loop |
| Prev | toggle | OFF / ON |
| Vel | 5 points | +/- `SEQ_STEP_VEL_ADJ_MAX` |
| Nudge | 1 tick | +/- `SEQ_STEP_NUDGE_MAX` |
| Taper | 5 % | +/- `SEQ_STEP_TAPER_MAX`, per ratchet sub-hit |

Every and Prev are independent conditions; both must hold for the step to
fire.

## Menu

Modal overlay. Every page shares one cursor.

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (wraps) |
| Encoder turn, editing | Change the row's value |
| Encoder click | Value row: enter / leave editing. Action row: run it. Dive row: open the sub-page. `< Back` row: leave the sub-page |
| SHOULDER (press) | On a sub-page: back to the main list, on the row it was entered from |
| Button 0 (tap / hold) | Cycle the active layer / play-stop (same as on Seq) |
| Button 3 (tap) | Close the menu |

Main list rows:

| Kind | Rows |
|---|---|
| Screen (runs and closes the menu) | Screen: Seq, Arp, Drone (the normal drone), Prog, FM (`CONFIG_SYNTH_CUSTOM_FM`), DEV (`CONFIG_SYNTH_DEV_MENU`) |
| Action (runs and closes the menu) | Add Layer, Del Layer |
| Value (click to edit) | BPM, Quant, Scale, Root, Arp, Drone, Stutter, Drum Bank, Volume (5 % steps) |
| Dive (open a sub-page) | Layer, Chords, Bounce, Prog Gen, FX, Projects (`CONFIG_SYNTH_PROJECT_STORE`), Wireless (`CONFIG_SYNTH_WIRELESS`) |

Sub-pages use the same click model: value rows edit on click, action rows run
on click.

| Page | Rows |
|---|---|
| Layer | Steps, Swing, Patch scope, Gate, Glide, Groove, Chord, Root, Type, Track, Follow, Repeat, Mute, Solo, ClrSolo (action, present only while something is soloed). Drum layers show `--` for melodic-only rows |
| Chords | Slot list CH1..CH8; a slot opens Root, Type, Clear. Every edit commits at once and auditions |
| Bounce | Shape rows (slot, bars, format, tail, after), Rec, Cancel, Undo, four rows per clip slot (play/mute, level, tempo, clear), Sample and its cancel |
| Prog Gen | Root, Scale, Arp Q, Style, Length, Bars, Ext, Var, Seed, Generate, Undo |
| FX | Hub: one dive row per AMY bus, plus the Preset FX guard. A bus row opens that bus's page: EQ, echo, chorus, reverb, distortion, Split, trim |
| Projects | Storage line, Templates.., then one row per slot (see below) |
| Wireless | BLE MIDI on/off (click), Status (read-only), Source (click toggles WAVE / PATCH), Patch (click to edit, turn cycles), Glide (click to edit, 1 ms per detent) |

### Projects page

| Step | Input |
|---|---|
| Enter a slot | Click the slot row. The action defaults to Load |
| Choose an action | Turn: Load, Save, Ren, Del, Exit |
| Run it | Click. Save on a used slot arms on the first click and saves on the second |
| Rename | Turn cycles the alphabet (A-Z, 0-9, space, `-`, `#`), click advances to the next character, `#` commits early |
| While naming | Button 1 saves, Button 2 discards |
| Open the templates | Click Templates..: `< Back` (returns to the Templates.. row), then one row per built-in template |
| Load a template | Click the template row (the action reads Load), click again. It replaces the current project; save it into a slot to keep edits. Templates have no Save, Ren or Del |

Results show inline in the slot or template row's value until the cursor
leaves it. A reopened Projects page comes back disarmed, on the project list.

## ADSR (graph) editor

Opened with SHIFT + Button 1. Opens on the EG0 (amp) page. Points are
selected and adjusted with the encoder.

| Input | Action |
|---|---|
| Encoder turn, selecting | Move between the envelope points (clamped at the ends) |
| Encoder click | Enter / leave adjusting the selected point. The sustain point cycles level, then time (when explicit decay is on), then back to selecting. In a sub-mode (below): leave the sub-mode |
| Encoder turn, adjusting | Change the point's value. Time steps scale with the segment length. The axis switches between a 2 s and a 15 s range on its own |
| Button 1 (press) | Cycle the shown envelope's curve type: Normal, Linear, DX7, TrueExp |
| Button 2 (press) | Cycle the sub-mode: off, AMP (amplitude trim, 5 % steps), SWG (layer swing; melodic rows reading the layer block, EG0 page only), then one stop per routing target PIT / CUT / DRV / MIX on rows carrying the depth matrix. The EG1 page starts straight on the target stops |
| SHOULDER (press) | Target stop up: flip the sign of that depth |
| Button 3 (tap) | Next editor: EG0, EG1, Filter, LFO, DIST, back to EG0. Targets without an EG1 page skip it |
| Button 0 (tap) | Commit and close |
| Button 0 (hold) | Cancel (restores the stored state) and close |
| SHIFT + Button 1 | Commit and close |
| SHIFT + Button 2 | Release the open tab (EG0 or EG1) to the patch |
| SHIFT + Button 3 | Flip the voice-block source |

Routing depth steps: 0.05 per detent on MIX, 0.25 octave on the others.
Live preview: edits sound at once; cancel restores.

## Filter editor

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (wraps): cutoff, resonance, then on KS patches feedback and pluck duty, then type, enable |
| Encoder click | Enter / leave editing the cursor row |
| Encoder turn, editing | Cutoff: 1 semitone per detent. Resonance: 0.02 of range. Feedback: log detents. Duty: 0.01. Type: LPF24, LPF, PHASER, NOTCH, HPF, BPF. Enable: toggles |
| Button 1 (press) | Toggle the filter on / off |
| Button 3 (tap) | Next editor |
| Button 0 (tap / hold) | Commit / cancel and close |
| SHIFT + Button 1 / 2 / 3 | Commit and close / release to patch / flip source |

The stutter drone's filter has only cutoff (sweep midpoint) and resonance.

## LFO editor

Not available on the stutter drone screen.

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (wraps) over the shown tab's target checkboxes, then Wave, Rate, Depth, Flt oct, Wobble rate, Wobble depth, Wobble mode, Enable |
| Encoder click | Checkbox, Enable, Wobble mode: toggle / cycle directly. Other rows: enter / leave editing |
| Encoder turn, editing | Wave and Rate: cycle. Depth: 1 % steps up to 10, then 5 %. Flt oct: quarter octaves. Wobble depth: whole dB |
| SHOULDER (tap) | Flip the target checklist between its two tabs (the second tab holds the distortion targets) |
| Button 3 (tap) | Next editor |
| Button 0 (tap / hold) | Commit / cancel and close |
| SHIFT + Button 1 / 2 / 3 | Commit and close / release to patch / flip source |

Button 1 does nothing here.

## DIST editor

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (wraps): Type, Drive, Bits, Rate, Mix |
| Encoder click | Enter / leave editing (every row is multi-value) |
| Encoder turn, editing | Type: cycle the eight stage sets (OFF is the bypass). Others: one step per detent |
| Button 3 (tap), Button 0 (tap / hold), SHIFT chords | As in the LFO editor |

## Arp screen

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (clamped) over Enable, Mode, Octaves, Rate, Gate, Portamento, Quant mode, then the slots |
| Encoder click | Enter / leave editing |
| Encoder turn, editing | Change the value. Gate: 5 % steps. Portamento: 1 ms. A slot: chromatic note; below the floor clears it; from empty, up seeds the root and down sets REST |
| Button 1 hold + turn | Cycle the arp's own patch |
| Button 0 (hold) | Play / stop. A tap does nothing |
| Button 2 | Ignored |
| Button 3 (tap) | Menu |
| SHIFT + Button 1 | Open the ADSR editor on the arp |

## Drone screens

Both are a scrollable parameter list.

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (clamped) |
| Encoder click | Enter / leave editing. Normal drone, STUTTER row: open the stutter drone. Stutter drone, visualiser row: open the visualiser; any click closes it and the encoder is inert there |
| Encoder turn, editing | Change the value |
| Button 1 hold + turn | Cycle the drone's patch |
| Button 0 (hold) | Play / stop. A tap does nothing |
| Button 2 | Ignored |
| Button 3 (tap) | Menu (the way back to the normal drone from the stutter drone) |
| SHIFT + Button 1 | Open the ADSR editor on the drone. Editors then cycle EG0, EG1, Filter, LFO, DIST (normal drone: no EG1 page; stutter drone: no LFO or DIST tab) |

## Prog screen

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (wraps): enable row, the entries, the apply-mode row |
| Encoder click | Enable row: toggle the progression. Apply row: toggle INST / BAR. Entry row: enter editing at Root; each further click moves Root, Type, Duration, then leaves |
| Encoder turn, editing | Root: 12 pitch classes. Type: chord types. Duration: 1, 2, 3, 4, 8, 16 bars |
| Button 1 (press) | Delete the entry under the cursor |
| Button 2 (press) | Append an entry |
| Button 0 (hold) | Play / stop. A tap does nothing |
| Button 3 (tap) | Menu |

While the progression is on, each melodic row follows it by its Layer page
Follow setting: CHORD voices the row onto the live chord (the default), ROOT
keeps the row's scale line and moves it by the nearest interval to each
chord's root, OFF ignores the progression.

## FM screen

Requires `CONFIG_SYNTH_CUSTOM_FM`. Two pages over the selected operator,
flipped with SHOULDER; each page keeps its own cursor.

Page 1: one cursor walks the six operator boxes (selecting as it goes), then
the panel rows RAT, LVL, TO, FB, ALG of the selected operator. The RAT row
shows `RAT 2.00` for a ratio operator and `FIX 440` for a fixed-frequency one,
and adjusts like page 2's coarse cell. The TO row shows `TO  OP1` for one
target and the target labels for several: `TO 1+2+3` up to three, `TO 1234`
above that. A muted operator's box is struck diagonally.

Page 2: the selected operator's frequency and 4-level envelope, `MUTE` in the
title while it is muted. Cursor stops, in order: OP, FRQ, coarse, fine, T1,
L1, T2, L2, T3, L3, T4, L4. From L4 a note rises to L1 over T1, then L2 over
T2, then L3 over T3, and holds L3; the release returns to L4 over T4. Levels
are DX7 levels (0.75 dB per step, 99 full, 0 silent).

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (clamped) |
| Encoder click, page 1 | On a box: jump to its RAT row. On FB when the selected operator is not the feedback operator: make it the feedback operator. On another row (or FB on the feedback operator): enter / leave adjusting |
| Encoder turn, adjusting, page 1 | RAT: coarse, as below. LVL: 5 %. TO: one target operator or OUT, replacing any set of targets. FB: 5 %, 0..120 %. ALG: step the algorithm |
| Encoder click, page 2 | FRQ: toggle RAT / FIX (seeded from the operator's pitch at A4). Other cells: enter / leave adjusting |
| Encoder turn, adjusting, page 2 | OP: select OP1..OP6. Coarse: ratio mode steps the curated ratios, keeping the fine offset; fixed mode 1 semitone. Fine: 0.1 Hz (ratio mode shown as Hz at A4). T1..T4: about 8 % of the time, at least 1 ms. L1..L4: 1 |
| Button 2 (press) | Mute / unmute the selected operator (not saved) |
| SHOULDER (tap) | Flip the page |
| Button 1 (press), page 1 | Start linking from the selected operator (below). Page 2: nothing |
| SHIFT + turn | Step the algorithm, as the ALG row does |
| Button 0 (tap / hold) | Cycle the active layer / play-stop |
| Button 3 (tap) | Menu |

Ratios clamp to 0.25..20, fixed frequencies to 1..9772 Hz.

### Linking

Linking sets which operators the selected operator (the source) modulates,
so one operator can modulate several. While linking, the title reads
`LINK OP<n>`, the source box and the link cursor's box are framed, and the
panel cursor is hidden.

| Input | Action |
|---|---|
| Encoder turn | Move the link cursor over the boxes (clamped) |
| Button 1 (press) | Click the box under the link cursor (rule below) |
| Button 3 (tap), encoder click | End linking |
| SHOULDER (tap) | End linking and flip the page |
| Button 2 (press) | Mute / unmute the source |
| SHIFT + turn | Ignored |

Clicking the source's own box does nothing. Until a click has been accepted,
a click replaces the source's targets with that operator, as TO does; this
includes a first click on a member of a fan-out seeded from a table
algorithm, which replaces the whole set. After that, a click on an operator
outside the set adds it, and a click on a member removes it unless it is
the only target.

Linking reaches operators only: making the source a carrier (OUT) stays on
the TO row. A click that would close a loop, or that needs a third
modulation bus, is refused and changes nothing; the title shows
`LINK: LOOP` or `LINK: NO BUS` until the next turn or click. Boxes whose
click would be refused have a dotted outline. A source that is the only
carrier refuses every link with LOOP, since a graph with no carrier is a
loop. An accepted click switches ALG to CUST and sounds at once.

## DEV screen

Requires `CONFIG_SYNTH_DEV_MENU`. Volatile controls and diagnostics; not
persisted.

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (wraps). Row 0 is `< back` (`< menu` on the root page) |
| Encoder click | Submenu row: enter. Action or toggle row: run it. Value row: enter / leave editing. Readout: nothing |
| Encoder turn, editing | Change the value |
| Button 3 (tap) | Menu |
