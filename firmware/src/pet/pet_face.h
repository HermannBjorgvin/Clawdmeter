#pragma once
#include <stdbool.h>

// Extra input for the big pet, beyond face.h. While the peek is up, a tap in
// the left or right third of the screen steps to the previous or next
// species instead of sending the pet home. True when the tap was used.
bool pet_face_tap(int x);
