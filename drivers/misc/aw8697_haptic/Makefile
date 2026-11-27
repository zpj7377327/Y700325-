# Y700 AW8697 haptic driver (clean rewrite, 2026-11-12)
# Only aw8697_y700.o is built. The old OPPO driver (aw8697.o +
# haptic.o = aw8692x.o + haptic_hv.o) is NOT built: aw8697_reg.h
# defines a non-static aw8697_reg_access[] table, so it may be
# included by exactly one .c file in this directory.
obj-$(CONFIG_AW8697_HAPTIC) += aw8697_y700.o
