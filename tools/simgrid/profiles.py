"""The panels the grid shows: one cell per distinct panel size and set of
board capabilities, labelled with the boards it stands for.

Each cell runs the simulator built at its size (SIM_LCD_W / SIM_LCD_H) with
the capability overrides the sim understands (SIM_CIRCLE, SIM_NO_PWR,
SIM_NO_TOUCH, SIM_MINIMAL, SIM_BUTTONS). Add a board here when its panel size
or capabilities aren't covered yet; boards that only share a cell go in its
`boards` list. A cell for a port that is still an open PR is marked
`on_main=False`.
"""

from dataclasses import dataclass, field


@dataclass(frozen=True)
class Profile:
    id: str                 # short, stable, used on the command line
    w: int
    h: int
    boards: tuple           # board names / envs this cell stands for
    round: bool = False     # BoardCaps.round_panel
    pwr: bool = True        # BoardCaps.has_pwr_button
    buttons: int = 1        # BoardCaps.button_count (1 = BOOT only)
    minimal: bool = False   # BoardCaps.minimal_ui
    touch: bool = True      # BoardCaps.has_touch
    on_main: bool = True    # False for a port that is still a PR

    @property
    def size(self):
        return f"{self.w}x{self.h}"

    @property
    def label(self):
        return f"{self.w}×{self.h}{' round' if self.round else ''} · {', '.join(self.boards)}"

    def env(self):
        """Environment overrides for this cell's simulator process."""
        e = {"SIM_BUTTONS": str(self.buttons)}
        if self.round:
            e["SIM_CIRCLE"] = "1"
        if not self.pwr:
            e["SIM_NO_PWR"] = "1"
        if self.minimal:
            e["SIM_MINIMAL"] = "1"
        if not self.touch:
            e["SIM_NO_TOUCH"] = "1"
        return e


PROFILES = (
    Profile("216", 480, 480, ("AMOLED-2.16", "AMOLED-2.16 C6", "SenseCap Indicator"),
            buttons=2),
    Profile("4848", 480, 480, ("4848S040",), pwr=False, buttons=0),
    Profile("lcd4", 480, 480, ("LCD-4",), pwr=False),
    Profile("18", 368, 448, ("AMOLED-1.8", "AMOLED-1.8 C6")),
    Profile("206", 410, 502, ("AMOLED-2.06",)),
    Profile("154", 240, 240, ("LCD-1.54",), buttons=2),
    Profile("154s", 240, 240, ("SpotPear S3 LCD-1.54",), pwr=False),
    Profile("round", 466, 466, ("AMOLED-1.32",), round=True),
    Profile("round143", 466, 466, ("AMOLED-1.43",), round=True, pwr=False),
    Profile("t4s3", 450, 600, ("LilyGO T4-S3",), pwr=False),
    Profile("cyd", 320, 240, ("ESP32-2432S028R", "ESP32-2432S024C")),
    Profile("fire", 320, 240, ("M5Stack FIRE",), buttons=2, touch=False),
    Profile("cydp", 240, 320, ("CYD portrait",)),
    Profile("lcd2", 240, 320, ("Waveshare LCD-2",), pwr=False, buttons=0),
    Profile("fnk", 240, 320, ("Freenove S3 Display (non-touch)",), buttons=0, minimal=True, touch=False),
    Profile("cube", 284, 240, ("Xingzhi Cube 1.83",), buttons=2, touch=False),
    Profile("lcd183", 240, 284, ("Waveshare LCD-1.83",)),
    Profile("tdisplay", 170, 320, ("LilyGO T-Display S3",), touch=False),
    Profile("stick", 135, 240, ("M5StickS3",), touch=False),
    Profile("epaper", 200, 200, ("ePaper 1.54",), minimal=True, touch=False),
)


def select(ids=None):
    """Profiles by id (comma list or iterable); all of them when None.
    `main` selects the boards on main."""
    if not ids:
        return list(PROFILES)
    if isinstance(ids, str):
        ids = [i for i in ids.split(",") if i]
    out = []
    for i in ids:
        if i == "main":
            out += [p for p in PROFILES if p.on_main and p not in out]
            continue
        match = [p for p in PROFILES if p.id == i or p.size == i]
        if not match:
            raise ValueError(f"unknown board {i!r}; known: {', '.join(p.id for p in PROFILES)}")
        out += [p for p in match if p not in out]
    return out
