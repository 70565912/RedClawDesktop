cmake_minimum_required(VERSION 3.20)

# Installer project scaffold. This does not build installer artifacts by default.
# Use redclaw_installer_build target explicitly after WiX is installed locally.

add_custom_target(redclaw_installer_plan
  COMMAND ${CMAKE_COMMAND} -E echo "Installer scaffold is present under installer/"
  COMMAND ${CMAKE_COMMAND} -E echo "Use installer/scripts/build-wix-installer.ps1 to build MSI after WiX setup."
)

add_custom_target(redclaw_installer_build
  COMMAND pwsh -File ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-wix-installer.ps1 -Configuration Release
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Build RedClaw installer using WiX"
)
