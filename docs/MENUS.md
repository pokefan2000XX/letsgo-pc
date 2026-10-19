# Menus

Let's Go's menus are NintendoWare layouts: each screen is a tree of panes (pictures, text
boxes, frames), with animations that move them and one file of textures. `tools/sarc.py` opens
the archives they come in, `tools/bflyt.py` lists a layout's panes and changes them, the
studio's texture tools change the textures, `tools/msg.py` the words and `tools/font.py` the
fonts. This is roadmap phase 8 ([PLAN.md](PLAN.md)).

## Where they are

- **Screens:** the 374 archives in `bin\app\<menu>\bin\` (`.arc`, SARC), for 56 menus: the
  main menu (`main_menu`), the Pokédex (`zukan`), the shop, the town map, the title screen and
  the rest.
- **Words:** the message files in `bin\message\<language>\common\`, one for each menu
  (`main_menu.dat`: `main_menu_01_03` is BAG). Most text boxes are empty in the layout; the menu's
  code fills them from its message file when it opens.

What an archive holds:

| Path in it | What |
|---|---|
| `blyt\*.bflyt` | the layouts (1,091 in all) |
| `anim\*.bflan` | their animations (4,363): panes sliding in, an icon changing when picked |
| `timg\__Combined.bntx` | every texture the archive's layouts use, in one BNTX |
| `fcpx\*.bfcpx` | the layouts' font settings: a font file's name and a size (not read yet) |
| `*.bnsh`, `*.bushvt` | shaders, in 21 of the archives |
| `*.flyt`, `*.flan` | the layout editor's own files (XML), which the game doesn't read. A text box's comment there often says which message it shows (`main_menu.mstxt:main_menu_01_01`) |

```
python -P tools/sarc.py list <file.arc>
python -P tools/sarc.py extract <file.arc> <outdir>
python -P tools/sarc.py replace <file.arc> <name> <file> <out.arc>
python -P tools/sarc.py roundtrip [files...]        every archive: read, write, compare
```

All 374 archives come back byte for byte.

## Panes

```
python -P tools/bflyt.py panes orig/romfs/bin/app/main_menu/bin/main_menu.arc:blyt/main_menu_00.bflyt
python -P tools/bflyt.py set <in> <out.bflyt> <pane> y=-144 scale_x=1.25 scale_y=1.25
python -P tools/bflyt.py animated <file.arc>:anim/main_menu_00_in.bflan
python -P tools/bflyt.py check [files.arc...]       walk every layout and animation
```

A pane is a null pane (`pan1`, a group to move together), a picture (`pic1`), a text box
(`txt1`), a frame (`wnd1`) or a part (`prt1`, another layout placed in this one, such as each
main menu button's icon). Each has a place (x, y, z in pixels from its parent's centre, y up),
a turn in degrees, a scale, a size, an alpha, and whether it shows. `set` changes those in
place, so the rest of the file stays as it was. The main menu's bottom row is `N_manu_01` to
`N_manu_05`: Pokédex, Bag, Party, Communicate and Save.

An animation overwrites the values it keys while it plays. The main menu's opening animations
key each button's x, so a button moved sideways jumps back as the menu opens; its y and scale
aren't keyed, so those can change. `animated` lists what each animation keys.

## Textures

The studio's texture tools work on the archive's BNTX once it's taken out:

```
pokebdsp lgpe textures --file __Combined.bntx
pokebdsp lgpe export --file __Combined.bntx --output <folder> [--as png|dds]
pokebdsp lgpe import --file __Combined.bntx --texture <name> --image <.png|.dds|.astc> --output <new.bntx>
```

The main menu's textures are BC2, BC4, BC5 and ASTC 4×4. The studio exports ASTC as a picture
but can't make one from a picture, so an ASTC texture is replaced with an `.astc` file. Names
keep the layout editor's suffix (`main_menu_03^p`), since the layouts name them that way.

## Fonts

The game's own fonts are in `bin\font\`, loaded when it starts: two OpenType fonts and a small
one (`.BFOTF`), and `num_00.bffnt`, a bitmap font. A `.BFOTF` is the font XORed with the key
of the Switch's system fonts after an 8-byte header (`tools/font.py`):

```
python -P tools/font.py decode <file.BFOTF> <out.otf>
python -P tools/font.py encode <file.otf | file.ttf> <out.BFOTF>
python -P tools/font.py roundtrip [files...]        every font: decode, encode, compare
```

`FOT-UDKakugoC80Pro-DB` draws the English text of the menus and the message boxes; replacing
`FOT-RodinNTLGPro-DB` changed nothing in the main menu, though the layouts' font settings
(`.bfcpx`) name it. The game takes TrueType fonts as well as OpenType ones.
`hacks/new-font/make_romfs.py --font <file>` puts a font of your choosing in place of the
first: with one from Windows, the main menu's labels, its button bar and a cutscene's message
box all showed in it (headless, 2026-10-17). The font has to have the characters the game
writes, and its licence has to let you use it; it stays on your PC with the hack's other
files.

## Putting it back

The mods folder reaches inside gfpaks but not inside SARCs ([MODS.md](MODS.md)), so a mod has
the whole `.arc`, with its changed files put back by `sarc.replace` and `sarc.write`. The rest
of the archive is the game's, so a hack commits the script that makes it from the player's copy.

## The check: an edited menu

`hacks/menu-edit/make_romfs.py` changes the main menu's Bag button three ways:

- **Layout:** the button (`N_manu_02`) 36 pixels higher and a quarter larger.
- **Texture:** its icon (`main_menu_03^p`, and `main_menu_03_passive^p` when it's picked) turned
  from yellow to purple. Exported as DDS with the studio, each BC2 block's two colours have their
  red, green and blue turned around (which turns every texel exactly), and imported back.
- **Text:** `main_menu_01_03` says BACKPACK instead of BAG.

```
python -P hacks/menu-edit/make_romfs.py [--studio <pokebdsp.exe>]
build/pc/letsgo.exe --mods hacks/menu-edit
```

Checked in the game (headless, 2026-10-17): from a copy of a save in Viridian Forest, the X
menu showed the Bag button raised, larger and purple, and BACKPACK under it when picked. At the
start of the game, before the partner Pokémon, only Save can be picked and the other buttons are
drawn pale, so their colours and names don't show there.

The studio's Menus page (letsgostudio) writes the same kind of archive. One written with the
page's code, with the Party button raised 40, 1.3 times larger and its icon's colours inverted,
showed that way in the X menu too (headless, same save, 2026-10-17).

The studio also adds a picture to a menu (`MenuLayout.WithNewPicture`): a copy of another
picture's pane and of its material, with a texture of its own, the layout rebuilt with every
other section the same. One added to the main menu, a copy of `P_monbo_00` in `N_menu_set_00`
showing a striped 256 × 96 picture as an 8-bit RGBA texture, showed at the top of the X menu
(headless, same save, 2026-10-17).

## Not yet

- Removing panes, adding any but pictures (the studio adds those), materials and their
  colours, text boxes' fonts, and animations' keys: those change the sections' sizes, so the
  file has to be rebuilt rather than changed in place.
- The bitmap font (`num_00.bffnt`), the archives' font settings (`.bfcpx`), and what
  `FOT-RodinNTLGPro-DB` and `beluga_font` draw.
- The menus' code (phase 8, step 3), for new screens or new behaviour.
- Replacing one file inside a SARC from the mods folder.
