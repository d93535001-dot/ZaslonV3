with open("zaslon_ui_manager.cpp", "r") as f:
    lines = f.readlines()

with open("zaslon_ui_manager.cpp", "w") as f:
    for line in lines:
        if "s.colors.WindowBg =" in line:
            f.write(line.replace("s.colors.WindowBg =", "float wbg[] =").split("//")[0] + "\n")
            f.write("    std::copy(std::begin(wbg), std::end(wbg), std::begin(s.colors.WindowBg));\n")
        elif "s.colors.Header =" in line:
            f.write(line.replace("s.colors.Header =", "float h[] =").split("//")[0] + "\n")
            f.write("    std::copy(std::begin(h), std::end(h), std::begin(s.colors.Header));\n")
        elif "s.colors.Accent =" in line:
            f.write(line.replace("s.colors.Accent =", "float a[] =").split("//")[0] + "\n")
            f.write("    std::copy(std::begin(a), std::end(a), std::begin(s.colors.Accent));\n")
        else:
            f.write(line)
