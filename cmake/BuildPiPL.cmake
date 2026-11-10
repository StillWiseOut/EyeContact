# Compiles an AE PiPL .r file into a Windows .rc, the same 3 steps the Adobe sample projects use:
#   cl /EP file.r > file.rr  ->  PiPLTool file.rr file.rrc  ->  cl /D MSWindows /EP file.rrc > file.rc
# Invoked with: cmake -DCL=... -DPIPLTOOL=... -DAE_HEADERS=... -DAE_RESOURCES=... -DIN=... -DOUT=... -DWORK=... -P BuildPiPL.cmake

get_filename_component(name "${IN}" NAME_WE)
set(rr  "${WORK}/${name}.rr")
set(rrc "${WORK}/${name}.rrc")

execute_process(
  COMMAND "${CL}" /nologo /I "${AE_HEADERS}" /I "${AE_RESOURCES}" /EP "${IN}"
  OUTPUT_FILE "${rr}" ERROR_VARIABLE log RESULT_VARIABLE res)
if(res)
  message(FATAL_ERROR "PiPL preprocess failed (${res}):\n${log}")
endif()

execute_process(
  COMMAND "${PIPLTOOL}" "${rr}" "${rrc}"
  OUTPUT_VARIABLE log ERROR_VARIABLE log RESULT_VARIABLE res)
if(res)
  message(FATAL_ERROR "PiPLTool failed (${res}):\n${log}")
endif()

execute_process(
  COMMAND "${CL}" /nologo /D MSWindows /EP "${rrc}"
  OUTPUT_FILE "${OUT}" ERROR_VARIABLE log RESULT_VARIABLE res)
if(res)
  message(FATAL_ERROR "PiPL .rc generation failed (${res}):\n${log}")
endif()
