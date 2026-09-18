# Content-addressed identity for the libEmuSC sources actually compiled in.
#
# LIBEMUSC_COMMIT is read at CMake CONFIGURE time, so a rebuild after a commit
# keeps the old string, and a build from an edited working tree reports the
# commit it was branched from as though nothing had changed. Both happened:
# P-0071 recorded two different binaries reporting the same version, and on
# 2026-08-28 a binary containing the partial-velocity-range fix still reported
# the commit before it, which nearly put a stale figure into a report.
#
# A hash of the source text cannot lie about either. Run at BUILD time, so it
# tracks edits without a reconfigure.
file(GLOB _srcs "${EMUSC_SRC_DIR}/*.cc" "${EMUSC_SRC_DIR}/*.h")
list(SORT _srcs)
set(_acc "")
foreach(_f ${_srcs})
  file(SHA256 "${_f}" _h)
  get_filename_component(_n "${_f}" NAME)
  string(APPEND _acc "${_h}  ${_n}\n")
endforeach()
string(SHA256 LIBEMUSC_SOURCE_SHA256 "${_acc}")
list(LENGTH _srcs LIBEMUSC_SOURCE_FILES)
configure_file("${VERSION_IN}" "${VERSION_OUT}" @ONLY)
