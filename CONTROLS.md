# Controls

User-facing control scheme: which physical input does what, on each screen,
overlay and editor. The gesture dispatcher is `input_dispatch_button()` and
`input_dispatch_encoder_steps()` in `main/input_dispatch.c`; per-screen input handlers are in
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
| SHOULDER (LB) | `MY_BUTTON_SHOULDER` | Left shoulder button. Acts on press, except the FM page flip, the WT keyframe cycle and the Mixer menu jump (tap) |
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
7. Mode screen: Arp, Stutter drone, Normal drone, Prog, FM, DEV, WT, Mixer, Seq (default at boot)

Screens are changed only through the Menu.

## Global gestures

| Gesture | Action |
|---|---|
| SHIFT + Button 0 (press) | Sequencer screen, no menu: toggle solo on the selected track. Arpeggiator screen, no menu: toggle the arp's solo. A SOLO / SOLO OFF banner shows the result. Elsewhere: nothing |
| SHIFT + Button 0 (hold) | Sequencer or Arpeggiator screen, no menu: clear every solo, the arp's included (SOLO CLR banner). Elsewhere: nothing |
| SHIFT + Button 1 | Open the ADSR editor on the current instrument (Seq, Arp, Drone screens, or the Wireless menu page). With an editor open: commit and close it |
| SHIFT + Button 2 | Sequencer screen, no menu: open the Step Trig popup. Popup open: close it. Filter/LFO/DIST/ADSR editor open: release the open tab to the patch |
| SHIFT + Button 3 | Sequencer screen, no menu: open the menu on the Layer page, on the row it was left on if the menu was closed on that page's layer entry, else on Gate. Arpeggiator screen, no menu: the same on the page's ARP entry, else on Level. Menu open on the Wireless page: switch to the Layer page's LIVE entry, on Level. Menu open on the Layer page: close it. Filter/LFO/DIST/ADSR editor open on a melodic row: flip the row between its own voice block and the layer's shared block |
| Button 3 (tap) | Toggle the menu; it reopens on the page and row it was closed on. With an editor open: next editor. With the Step Trig popup open: close it |

A SHIFT chord is latched on its digit's press and swallows the rest of that
press, so the button's own tap/hold gesture never fires. A chord whose
preconditions are not met does nothing.

## Hint strip

The bottom line shows `1:<b1> 2:<b2> 3:<b3>` and, in the menu on a sub-page,
` LB:Main`; on the FM screen ` LB:Pg2` or ` LB:Pg1` (the page SHOULDER flips
to); on the WT screen ` LB:Key`; on the Mixer screen ` LB:Main`. It is read from `ui_view_table[]` (`ui_view_resolve.c`) for the
active view. SHIFT chords are not shown. The strip is hidden on the Prog
screen; a DEV status bar replaces it on every screen while on.

| View | 1 | 2 | 3 |
|---|---|---|---|
| Seq | Patch | Pitch | Menu |
| Arp, Stutter, Normal drone | Patch | - | Menu |
| FM | Link (page 1), - (page 2) | Mute | Menu (Done while linking) |
| WT | Copy (- on HRM and the S tab) | Reset (Blend on keyframe M, off HRM; - on the S tab) | Menu |
| Mixer | Mute | Param (List in PARAM mode) | Menu (Back in a layer's track rows) |
| Menu | - (Save while naming) | - (Disc while naming) | Menu |
| Prog | Del | +Add | Menu |
| DEV | - | - | Menu |
| ADSR editor | Type | Amp / Swg / Pit / Cut / Drv / Mix / Off (the next stop) | Next |
| Filter editor | On/Off | - | Next |
| LFO editor | On/Off | +LFO2 while LFO2 is collapsed, -LFO2 while it is open (- where LFO2 is unavailable) | Next |
| DIST editor | - | - | Next |
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
| SHIFT + Button 0 (press / hold) | Toggle solo on the selected track / clear every solo (Global gestures) |
| Button 3 (tap) | Menu |

## Step Trig popup

Addressed by the grid cursor (layer, track, step). Eight fields in a
five-row window that scrolls with the cursor; a ninth, FRM, is listed while
the track plays a wavetable patch.

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
| FRM | 1 frame | `--` (no lock), then 0..63: the wavetable frame this step plays, overriding the Layer page's Frame; ratchet sub-hits share it |

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
| Screen (runs and closes the menu) | Sequencer, Arpeggiator, Drones (the normal drone, with the stutter drone behind its STUTTER row), Progression, FM (`CONFIG_SYNTH_CUSTOM_FM`), Wavetable (`CONFIG_SYNTH_CUSTOM_WT`), DEV (`CONFIG_SYNTH_DEV_MENU`) |
| Action (runs and closes the menu) | Add Layer, Del Layer |
| Value (click to edit) | BPM, Quant, Scale, Root, Arp, Drone, Stutter, Drum Bank, Volume (5 % steps) |
| Dive (open a sub-page) | Layer, Chords, Bounce, Prog Gen, FX, Projects (`CONFIG_SYNTH_PROJECT_STORE`), Wireless (`CONFIG_SYNTH_WIRELESS`) |

Sub-pages use the same click model: value rows edit on click, action rows run
on click.

Layer page Frame row: present only while the Track row's track plays a
wavetable patch. The wavetable frame each note starts on: `Auto` (the resting
position the EG -> SCN routing implies: the first frame, the last, or the
middle when unrouted), then 0..63, one frame per detent with Auto below 0.
EG -> SCN and LFO SCAN modulate around it, and a step's FRM lock overrides
it. Like Unison it follows the patch scope: LAYER scope sets every track,
TRACK scope the Track row's track. With a Square LFO on the row, a Frame or
FRM between 1 and 62 also narrows the LFO square into a pulse of that width
(`amy_helpers_note_send_duty` in `amy_helpers.h`).

| Page | Rows |
|---|---|
| Layer | Title `LAYER L<layer>/<layers> T<track>`. Track (the grid's selected track: turning it moves the cursor's row, and the rows below it edit that row), Level (the Track row's volume, 0-100 % in 5 % steps), Key (filter key tracking, 0-100 %, pivot = main menu Root in octave 4; melodic layers), Follow, Repeat, Mute, Solo, ClrSolo (action, present only while something is soloed), Frame (wavetable tracks, see above), Unison (dive, melodic), PCM (dive, drum layer on the PCM engine), then the layer-wide rows Steps, Swing, Patch scope, Gate, Glide, Groove, Chord, Root, Type. Drum layers show `--` for melodic-only rows. The page has two more entries, reached with SHIFT + Button 3. ARP (from the arp screen): Level, Key, Mute, Solo, ClrSolo, Unison (dive), Gate (10-100 %, 5 % steps), Glide. Mute silences the arp without switching it off; Solo makes the arp a member of the solo set, so it plays alongside soloed rows and mutes everything unsoloed. LIVE (from the Wireless page): Level, Key, Unison (dive), Glide; `< Back` returns to the Wireless page. Their Unison page edits the arp's or live voice's own unison, with the same rows and limits as a track's (the arp's is saved with the project, the live voice's lasts the session) |
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
| Button 2 (press) | Cycle the sub-mode: off, AMP (amplitude trim, 5 % steps; live voice and drones only - a sequencer row sets its level on the Layer page's Level row, the arp on the ARP entry's Level row), SWG (layer swing; melodic rows reading the layer block, EG0 page only), then one stop per routing target PIT / CUT / DRV / MIX on rows carrying the depth matrix. The EG1 page starts straight on the target stops |
| SHOULDER (press) | Target stop up: flip the sign of that depth |
| Button 3 (tap) | Next editor: EG0, EG1, Filter, LFO1, LFO2 (while open), DIST, back to EG0. Targets without an EG1 page skip it |
| Button 0 (tap) | Commit and close |
| Button 0 (hold) | Cancel (restores the stored state) and close |
| SHIFT + Button 1 | Commit and close |
| SHIFT + Button 2 | Release the open tab (EG0 or EG1) to the patch |
| SHIFT + Button 3 | Flip the voice-block source |

Routing depth steps per detent: on PIT 0.01 octave below 0.2, 0.05 from 0.2 to 1.0, 0.25 from 1.0 (turning back toward 0 takes the finer step at a boundary: 1.00 -> 0.95, 0.20 -> 0.19); 0.05 on MIX and SCN, 0.25 octave on CUT and DRV.
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

## LFO editor

Every voice has two LFOs. The editor opens on LFO1. LFO2 is collapsed by
default; Button 2 opens it, which adds an LFO2 page after LFO1 (Button 3
steps LFO1, LFO2, then DIST). Open and on are separate: Button 2 opens and
collapses LFO2, Button 1 or the EN chip turns the shown LFO on and off. A
collapsed LFO2 keeps its settings, and both LFOs are saved with the project.
LFO2 needs a patch with an LFO target map (not the piano); elsewhere Button 2
does nothing.

While FILTER is a checked target and the surface's filter is on, the header
shows the cutoff range the LFO sweeps, as low-high Hz (e.g. "550-2.2k").
While LFO2 is open, the LFO1 header shows a "2" chip left of EN: inverted
while LFO2 runs (on, with targets), outlined while it is off. The LFO2 page
has the same layout under the title LFO2. On a Juno or DX7 patch, whose own
LFO uses the slot LFO2 takes, the LFO2 page shows a struck "P" chip: LFO2
replaces the patch LFO while it is open, also while it is off. Collapsing
LFO2 gives the patch LFO back (a sequencer row reloads its layer for it).

Each target belongs to one LFO. A target the other LFO drives shows that
LFO's number ("1" or "2") instead of its checkbox; checking it moves the
target to this LFO.

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (wraps) over the seven target checkboxes (Filter, Amp, Pitch, Pan, Scan, Drive, Mix; the list scrolls, five rows at a time), then Wave, Rate, Depth, Flt oct, Wobble rate, Wobble depth, Wobble mode, Enable |
| Encoder click | Checkbox, Enable, Wobble mode: toggle / cycle directly. Other rows: enter / leave editing |
| Encoder turn, editing | Wave and Rate: cycle. Depth: 1 % steps up to 10, then 5 %. Flt oct: quarter octaves. Wobble depth: whole dB |
| Button 1 (press) | Turn the shown LFO on / off |
| Button 2 (press) | LFO1 page: open LFO2 (it turns on; its settings are kept) or collapse it. LFO2 page: collapse LFO2 and return to LFO1 |
| Button 3 (tap) | Next page or editor: LFO1, LFO2 while open, then DIST |
| Button 0 (tap / hold) | Commit / cancel and close |
| SHIFT + Button 1 / 2 / 3 | Commit and close / release to patch (on the LFO2 page: collapse LFO2) / flip source |

SHOULDER does nothing here.

## DIST editor

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (wraps): Type, Drive, Bits, Rate, Mix |
| Encoder click | Enter / leave editing (every row is multi-value) |
| Encoder turn, editing | Type: cycle the eight stage sets (OFF is the bypass). Others: one step per detent |
| Button 3 (tap), Button 0 (tap / hold), SHIFT chords | As in the LFO editor |

## Mixer screen

One row per sound source: MAST (master volume), DRUM (layer 0), L2..L4 (the
melodic layers), ARP, DRON (normal drone), STUT (stutter drone), and LIVE and
CLP1 / CLP2 while the live voice and those clip slots exist, then FADE, the
fade length (overview only). A layer row dives
into its track rows (the drum layer's by role, LOW..AUX; a melodic layer's
T1..T5). Each row shows a bar (the fader position) and an M cell: `M` filled
for the mixer's mute, `m` when only the Layer page's (or the arp's) own mute
is on. While a row fades, a tick on its bar marks the target. Two quick
cells follow (overview source rows only), each a fixed setting of the row's
source; the cell shows the value and, while it is selected in PARAM mode,
the title shows its name (`MIXER  CUT`):

| Row | Q1 | Q2 |
|---|---|---|
| Melodic layer | CUT: every row's filter cutoff, a semitone per detent | GAT: layer gate, the Layer page's Gate steps |
| Drum layer | SWG: layer swing, 2 per detent, 0..66 | - |
| ARP | OCT: octaves 1..4 | GLD: glide ms, 5 per detent, 0..100 |
| STUT | PAT: gate pattern | RT: root note |
| DRON | RT: root note | CUT: filter cutoff, a semitone per detent |
| LIVE | GLD: glide ms, 5 per detent, 0..100 | CUT: filter cutoff, a semitone per detent |

| Input | Action |
|---|---|
| Encoder turn | LIST mode: move the cursor (clamped); five rows show at a time. PARAM mode, not editing: select among the cursor row's quick cells (clamped, never leaves the row). PARAM mode, editing: step the selected cell (table above), clamped |
| SHIFT hold + turn | Fader position of the cursor row in 5 % steps, 0..100 (MAST included), in either mode; cancels a fade on that row. FADE row: the fade length, one step per detent through 1, 2, 4, 8, 16, 32 bars. The bar is inverted while SHIFT is held |
| Encoder click | LIST mode: on a layer row, open its track rows; other rows, nothing. PARAM mode: start / stop editing the selected cell (outlined while selected, filled while editing) |
| Button 1 (press) | Toggle the mixer mute of the cursor row (not MAST, not FADE) |
| Button 2 (press) | Toggle LIST <-> PARAM mode on the cursor row. Nothing on a row without quick cells (MAST, FADE, the clips, track rows). Entering selects the first cell, not editing |
| SHIFT + Button 2 | Fade the cursor row (MAST included, not FADE): from above 0, to 0 over the FADE length from the next bar line (at once while stopped); from 0, back to where the last fade-out started (100 if none). During a fade that has not moved yet (waiting for the bar line): flip it, an out becomes a cut to silence and a fade in from 0 to the old level, an in becomes an out from that level; once it moves: turn it around from where it is, starting at once |
| Button 3 (tap) | In a layer's track rows: back to the overview, on the row it was opened from, in LIST mode. Elsewhere: menu |
| SHOULDER (tap) | Open the menu on its main list, from anywhere on the page; an open dive stays, so returning lands where you were |
| Button 0 (tap / hold) | Cycle the active layer / play-stop |

PARAM mode stays inside the cursor row: the turn cannot move to another row
until Button 2 returns to LIST mode.

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
| SHIFT + Button 0 (press / hold) | Toggle the arp's solo / clear every solo (Global gestures) |
| SHIFT + Button 1 | Open the ADSR editor on the arp |

## Drone screens

Both are a scrollable parameter list.

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (clamped) |
| Encoder click | Enter / leave editing. Normal drone, STUTTER row: open the stutter drone |
| Encoder turn, editing | Change the value |
| Button 1 hold + turn | Cycle the drone's patch |
| Button 0 (hold) | Play / stop. A tap does nothing |
| Button 2 | Ignored |
| Button 3 (tap) | Menu (the way back to the normal drone from the stutter drone) |
| SHIFT + Button 1 | Open the ADSR editor on the drone. Editors then cycle EG0, EG1, Filter, LFO1, LFO2 (while open), DIST |

Each drone's PATCH row names its patch and, while editing, cycles the same
patch list as Button 1 hold + turn. On the stutter drone, LEVEL and DUCK set
the gate's on and off levels, and the PATTERN row draws its 8 steps (filled =
open). Its filter, its sweep (the LFO editor's FILTER target) and its
distortion live in the shared editors.

The FOLLOW row after CHORD (both drones, wraps) sets how the drone follows
the chord progression: OFF plays its own ROOT and CHORD, ROOT moves its own
chord shape onto each progression chord's root nearest its ROOT, CHORD does
the same with the progression chord's type (the CHORD row then reads
"(prog)").

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
title while it is muted. Cursor stops, in order: OP, FRQ, coarse, fine, R1,
L1, R2, L2, R3, L3, R4, L4. From L4 a note moves to L1 at rate R1, then to L2
at R2, then to L3 at R3, and holds L3; the release returns to L4 at R4. Levels
are DX7 levels (0.75 dB per step, 99 full, 0 silent). Rates are DX7 rates
(0..99, higher is faster; six steps halve the time). A rate is a slope: at the
same rate a segment between levels close together is over sooner than one
between levels far apart, and a segment between equal levels takes no time.
The plot shows the resulting shape.

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (clamped) |
| Encoder click, page 1 | On a box: jump to its RAT row. On FB when the selected operator is not the feedback operator: make it the feedback operator. On another row (or FB on the feedback operator): enter / leave adjusting |
| Encoder turn, adjusting, page 1 | RAT: coarse, as below. LVL: 5 %. TO: one target operator or OUT, replacing any set of targets. FB: 5 %, 0..120 %. ALG: step the algorithm |
| Encoder click, page 2 | FRQ: toggle RAT / FIX (seeded from the operator's pitch at A4). Other cells: enter / leave adjusting |
| Encoder turn, adjusting, page 2 | OP: select OP1..OP6. Coarse: ratio mode steps the curated ratios, keeping the fine offset; fixed mode 1 semitone. Fine: 0.1 Hz (ratio mode shown as Hz at A4). R1..R4: 1. L1..L4: 1 |
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

## WT screen

Requires `CONFIG_SYNTH_CUSTOM_WT`. Edits the one custom wavetable, patch 288
(`Wavetable: Custom`), shared by every row, the arp and the drones on that
patch and saved with the project. The table morphs through three keyframes:
A (frame 0), M (frame 32) and B (frame 63). Each keyframe has five rows, plus
one global row (HRM):

| Row | Range | Meaning |
|---|---|---|
| SHP | 0..100 | Shape: the synced waveform, saw at 0 to square at 100 |
| WID | 10..90 | Width: pulse width of the square part in percent, 50 = symmetric square. No effect at SHP 0 |
| BRT | 0..10 | Bright: slope above the sync harmonic; 10 leaves the waveform as is, 0 rolls off steeply. |
| SYN | 1.0..8.0 | Hard-sync ratio; whole numbers are that many waveform periods per cycle. |
| PK | off, 2..63 | +12 dB formant bump on that harmonic |
| HRM | 63 / 31 / 15 | Harmonics: how many harmonics every frame may hold, shown with the highest note that then plays without aliasing: `63 F#4`, `31 F#5`, `15 G6`. More harmonics is brighter and lowers that note. Notes above G6 alias at every setting. The cell has no label on screen |

The screen has no title. The top band holds, left of a vertical border, the
tabs `A M B S` (the focused one filled) above the harmonics cell, and right
of it the focused keyframe's rows: SHP and WID on the first line, BRT, SYN
and PK on the second. Below, the waveform spans the full width: the focused
keyframe as a line, the other two as dotted traces. It updates when the
rebuild after an edit finishes. Cursor stops, in order: SHP, WID, BRT, SYN,
PK, HRM; they apply to the focused keyframe.

The S tab is a scan viewer: the parameter cells give way to a `FRAME`
readout and a ruler with ticks on frames 0, 32 and 63, and the waveform
shows that one frame of the table with no other trace. A click swaps the
waveform for the frame's harmonics as bars: harmonic 1 on the left up to the
harmonic count (63, 31 or 15 bars, widened to fill the screen), 60 dB from
top to bottom, dotted lines 20 and 40 dB below full scale. The tab only
changes what is drawn; no parameter and no sounding note follows it.

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (clamped) |
| Encoder click | Enter / leave adjusting |
| Encoder turn, adjusting | Change the value by one step (SYN 0.1, WID 1); PK steps off <-> 2 |
| SHOULDER (tap) | Focus the next tab, A -> M -> B -> S -> A; the cursor stop, adjusting state and scan frame stay |
| Encoder turn, S tab | Step the displayed frame, 0..63 (clamped). Button 1 and Button 2 do nothing there |
| Encoder click, S tab | Flip between the frame's waveform and its harmonics |
| Button 1 (press) | Copy the focused keyframe to the next one: A -> M, M -> B, B -> A. HRM: nothing |
| Button 2 (press) | A or B: reset it to the saw (SHP 0, WID 50, BRT 10, SYN 1.0, PK off). M: set it to the halfway blend of A and B, which makes the table a plain A-to-B morph. HRM: back to 31 harmonics |
| SHIFT + turn | Same as a plain turn |
| Button 0 (tap / hold) | Cycle the active layer / play-stop |
| Button 3 (tap) | Menu |

## DEV screen

Requires `CONFIG_SYNTH_DEV_MENU`. Volatile controls and diagnostics; not
persisted.

| Input | Action |
|---|---|
| Encoder turn, browsing | Move the cursor (wraps). Row 0 is `< back` (`< menu` on the root page) |
| Encoder click | Submenu row: enter. Action or toggle row: run it. Value row: enter / leave editing. Readout: nothing |
| Encoder turn, editing | Change the value |
| Button 3 (tap) | Menu |
