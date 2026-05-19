with open("zaslon_ui_manager.h", "r") as f:
    content = f.read()
content = content.replace("const char* GetString(const std::string& key);", "const char* GetString(const char* key);")
with open("zaslon_ui_manager.h", "w") as f:
    f.write(content)

with open("zaslon_ui_manager.cpp", "r") as f:
    content = f.read()
content = content.replace("const char* Localization::GetString(const std::string& key) {", "const char* Localization::GetString(const char* key) {")
with open("zaslon_ui_manager.cpp", "w") as f:
    f.write(content)
