// Clawdmeter Desk Stand
// Parametric kickstand for Waveshare ESP32-C6-Touch-AMOLED-2.16
// Two pieces: cradle (snap-fit) + leg (kickstand with detent hinge)
// Hardware: 1x M3x20mm bolt + 1x M3 hex nut + 1x 10x10mm rubber pad

// ── RENDER CONTROL ──────────────────────────────────────────────────────────
// Change RENDER to export each piece:
//   "cradle"   → export clawdmeter_cradle.stl
//   "leg"      → export clawdmeter_leg.stl
//   "assembly" → preview both assembled (not for export)
RENDER = "assembly";

// ── DEVICE DIMENSIONS (Waveshare C6-Touch-AMOLED-2.16 datasheet) ────────────
DEVICE_W     = 46.0;   // PCB width  [mm]
DEVICE_H     = 46.0;   // PCB height [mm]
DEVICE_D     = 8.3;    // PCB depth  [mm]
DEVICE_R     = 5.8;    // PCB corner radius [mm]

// ── CRADLE PARAMETERS ────────────────────────────────────────────────────────
CRADLE_CLEAR = 0.3;    // clearance per side for snap-fit [mm]
WALL         = 2.5;    // cradle wall thickness [mm]
SNAP_TAB_H   = 1.5;    // snap tab retention depth [mm]
SNAP_TAB_L   = 14.0;   // snap tab length [mm]
USBC_W       = 14.0;   // USB-C cutout width [mm]
USBC_H       = 5.0;    // USB-C cutout height [mm]

// ── HINGE PARAMETERS ─────────────────────────────────────────────────────────
HINGE_R      = 14.0;   // detent disc radius [mm]
HINGE_TEETH  = 36;     // teeth count → 10° per step
TOOTH_H      = 1.2;    // tooth height above disc face [mm]
EAR_T        = 4.0;    // ear thickness (each side) [mm]
EAR_GAP      = 0.4;    // clearance between ear and leg disc [mm]

// ── LEG PARAMETERS ───────────────────────────────────────────────────────────
LEG_L        = 80.0;   // leg length [mm]
LEG_W        = 12.0;   // leg width [mm]
LEG_T        = 4.0;    // leg thickness [mm]
PAD_W        = 10.0;   // rubber pad width [mm]
PAD_L        = 10.0;   // rubber pad length [mm]
PAD_D        = 1.5;    // rubber pad recess depth [mm]

// ── HARDWARE ─────────────────────────────────────────────────────────────────
M3_D         = 3.4;    // M3 bolt hole diameter (with clearance) [mm]
M3_NUT_W     = 6.1;    // M3 hex nut flat-to-flat + 0.1mm clearance [mm]
M3_NUT_H     = 2.5;    // M3 hex nut height + 0.1mm [mm]

// ── DERIVED ──────────────────────────────────────────────────────────────────
CW = DEVICE_W + 2*CRADLE_CLEAR + 2*WALL;  // cradle outer width
CH = DEVICE_H + 2*CRADLE_CLEAR + 2*WALL;  // cradle outer height
CD = DEVICE_D + WALL;                      // cradle outer depth (open back)

// ── SANITY CHECKS ─────────────────────────────────────────────────────────────
echo("Cradle outer WxH:", CW, CH);
echo("Hinge bolt span:", 2*(EAR_T+EAR_GAP)+LEG_T);
assert(2*(EAR_T+EAR_GAP)+LEG_T < 20,
       "Hinge stack exceeds M3x20 bolt — reduce EAR_T or LEG_T");
assert(HINGE_R > DEVICE_D/2 + 2,
       "Hinge disc too small — cradle may collide with desk");

$fn = 64;

// ── DISPATCH ─────────────────────────────────────────────────────────────────
if      (RENDER == "cradle")   cradle();
else if (RENDER == "leg")      leg();
else                           assembly();

// ── HELPERS ──────────────────────────────────────────────────────────────────

module rounded_box(w, h, d, r) {
    hull() {
        for (xi = [r, w-r], yi = [r, h-r])
            translate([xi, yi, 0]) cylinder(r=r, h=d);
    }
}

module rounded_pocket(w, h, d, r) {
    hull() {
        for (xi = [r, w-r], yi = [r, h-r])
            translate([xi, yi, -0.5]) cylinder(r=r, h=d+1);
    }
}

// ── CRADLE ───────────────────────────────────────────────────────────────────
// Orientation: device face at Z=0 (facing user), bottom (USB-C side) at Y=0
// Hinge ears extend from Y=0 face downward (negative Y)

module cradle() {
    difference() {
        // Outer shell
        rounded_box(CW, CH, CD, WALL);

        // Device pocket (open on front face, Z=0)
        translate([WALL - CRADLE_CLEAR, WALL - CRADLE_CLEAR, WALL])
            rounded_pocket(
                DEVICE_W + 2*CRADLE_CLEAR,
                DEVICE_H + 2*CRADLE_CLEAR,
                CD,
                DEVICE_R + CRADLE_CLEAR);

        // Top opening — remove top wall for button access
        translate([-1, CH - WALL, -1])
            cube([CW + 2, WALL + 1, CD + 2]);

        // USB-C cutout at bottom center (Y=0 wall)
        translate([CW/2 - USBC_W/2, -1, WALL + CRADLE_CLEAR])
            cube([USBC_W, WALL + 2, USBC_H]);

        // M3 bolt channel through both ears (Y axis)
        translate([CW/2, -(EAR_T + EAR_GAP + 1), CD/2])
            rotate([-90, 0, 0])
            cylinder(d=M3_D, h=2*(EAR_T + EAR_GAP) + LEG_T + 2);
    }

    // Snap tabs — left and right
    snap_tab(true);
    snap_tab(false);

    // Hinge ears at bottom
    hinge_ears();
}

module snap_tab(left) {
    x_base  = left ? -WALL : CW;
    flip    = left ? 0 : 1;
    y_start = (CH - SNAP_TAB_L) / 2;

    translate([x_base, y_start, 0])
    mirror([flip, 0, 0])
    difference() {
        cube([WALL, SNAP_TAB_L, CD]);
        // 45° lead-in chamfer at the tip (Z=CD end)
        translate([-0.1, -0.1, CD - SNAP_TAB_H * 2])
            rotate([0, 45, 0])
            cube([WALL * 2, SNAP_TAB_L + 0.2, WALL * 2]);
    }
}

module hinge_ears() {
    for (side = [-1, 1]) {
        x_pos = CW/2 + side * (LEG_T/2 + EAR_GAP + EAR_T/2);
        translate([x_pos, 0, CD/2])
        rotate([90, 0, 0])   // disc normal → Y axis
        difference() {
            cylinder(r=HINGE_R, h=EAR_T, center=true);
            // M3 bolt hole
            cylinder(d=M3_D, h=EAR_T + 1, center=true);
            // Tooth slots (negative teeth, offset by half pitch to mesh)
            for (i = [0:HINGE_TEETH-1])
                rotate([0, 0, i * 360/HINGE_TEETH + 180/HINGE_TEETH])
                translate([HINGE_R - TOOTH_H - 0.3, 0, 0])
                cylinder(r=1.1, h=EAR_T + 1, center=true, $fn=3);
        }
    }
}

// ── LEG ──────────────────────────────────────────────────────────────────────
// Printed flat. Disc at Y=0 end, foot at Y=LEG_L end.
// Bolt axis = Z when printed; becomes Y in assembly.

module leg() {
    union() {
        // Main arm
        translate([0, HINGE_R, 0])
            cube([LEG_W, LEG_L - HINGE_R - 18, LEG_T]);

        // Detent disc at top (Y=0)
        translate([LEG_W/2, 0, LEG_T/2])
        rotate([90, 0, 0])
        difference() {
            union() {
                cylinder(r=HINGE_R, h=LEG_T, center=true);
                // Raised teeth
                for (i = [0:HINGE_TEETH-1])
                    rotate([0, 0, i * 360/HINGE_TEETH])
                    translate([HINGE_R - TOOTH_H/2, 0, 0])
                    cylinder(r=1.0, h=LEG_T + 0.2, center=true, $fn=3);
            }
            // M3 bolt through-hole
            cylinder(d=M3_D, h=LEG_T + 1, center=true);
            // M3 nut pocket on outer face
            translate([0, 0, -(LEG_T/2 - M3_NUT_H)])
                cylinder(d=M3_NUT_W / cos(30), h=M3_NUT_H + 0.5,
                         center=false, $fn=6);
        }

        // Foot with rubber pad recess
        translate([0, LEG_L - 18, 0])
        difference() {
            translate([-3, 0, 0]) cube([LEG_W + 6, 18, LEG_T + 2]);
            translate([(LEG_W + 6)/2 - PAD_W/2 - 3,
                       4,
                       LEG_T + 2 - PAD_D])
                cube([PAD_W, PAD_L, PAD_D + 1]);
        }
    }
}

// ── ASSEMBLY PREVIEW ─────────────────────────────────────────────────────────
TILT_ANGLE = 60;  // 0=folded flat, 90=upright

module assembly() {
    cradle();
    // Hinge center on cradle: [CW/2, 0, CD/2]
    // Leg disc center matches when placed at origin, then translated
    translate([CW/2, 0, CD/2])
    rotate([TILT_ANGLE - 90, 0, 0])
    translate([-LEG_W/2, -HINGE_R, -LEG_T/2])
        leg();
}
