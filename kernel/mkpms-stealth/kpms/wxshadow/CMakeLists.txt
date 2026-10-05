# Kernel objects are linked into ../stealth/stealth.kpm.
# Build client tool (userspace)
add_executable(wxshadow_client wxshadow_client.c)
target_link_options(wxshadow_client PRIVATE -static -s)
