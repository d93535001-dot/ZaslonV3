import re

with open("zaslon_deep_enhancement.cpp", "r") as f:
    content = f.read()

# Fix bounds checking in PE Inspector
new_pe_logic = """
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart < sizeof(IMAGE_DOS_HEADER)) {
        UnmapViewOfFile(pBase);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return std::nullopt;
    }

    PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)pBase;
    if (pDosHeader->e_magic == IMAGE_DOS_SIGNATURE) {
        // Bounds check before accessing NT headers
        if (pDosHeader->e_lfanew > 0 && pDosHeader->e_lfanew < (fileSize.QuadPart - sizeof(IMAGE_NT_HEADERS32))) {
            PIMAGE_NT_HEADERS pNtHeaders = (PIMAGE_NT_HEADERS)((uint8_t*)pBase + pDosHeader->e_lfanew);
            if (pNtHeaders->Signature == IMAGE_NT_SIGNATURE) {
"""

content = re.sub(
    r'PIMAGE_DOS_HEADER pDosHeader = \(PIMAGE_DOS_HEADER\)pBase;\s*if \(pDosHeader->e_magic == IMAGE_DOS_SIGNATURE\) \{\s*PIMAGE_NT_HEADERS pNtHeaders = \(PIMAGE_NT_HEADERS\)\(\(uint8_t\*\)pBase \+ pDosHeader->e_lfanew\);\s*if \(pNtHeaders->Signature == IMAGE_NT_SIGNATURE\) \{',
    new_pe_logic,
    content
)

# Fix Hardware Telemetry Handle count parsing
new_hw_logic = """
                NTSTATUS status = pNtQuerySystemInformation(SystemHandleInformation, buffer.data(), returnLength, &returnLength);
                if (status == 0) {
                    // SystemHandleInformation struct starts with a ULONG for the count of handles
                    if (buffer.size() >= sizeof(ULONG)) {
                        metrics.active_handles = *(ULONG*)buffer.data();
                    }
                }
"""

content = re.sub(
    r'NTSTATUS status = pNtQuerySystemInformation\(SystemHandleInformation, buffer.data\(\), returnLength, &returnLength\);\s*if \(status == 0\) \{\s*metrics.active_handles = 1500;\s*\}',
    new_hw_logic,
    content
)

with open("zaslon_deep_enhancement.cpp", "w") as f:
    f.write(content)
