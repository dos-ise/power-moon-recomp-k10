// Quick check: parse NSO files and print the same summary as tools/nso.py's __main__.
#include "nso.h"

#include <cstdio>
#include <map>

using namespace ngen;

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        try {
            Nso n(argv[i]);
            printf("== %s  build %.16s  image 0x%llx\n", argv[i], n.build_id.c_str(),
                   (unsigned long long)n.size);
            printf("  text 0x%llx+0x%llx  ro 0x%llx+0x%llx  data 0x%llx+0x%llx  bss 0x%x\n",
                   (unsigned long long)n.text.first, (unsigned long long)n.text.second,
                   (unsigned long long)n.rodata.first, (unsigned long long)n.rodata.second,
                   (unsigned long long)n.data.first, (unsigned long long)n.data.second,
                   n.bss_size);
            printf("  needed:");
            for (auto &s : n.needed) printf(" %s", s.c_str());
            printf("  syms %zu  imports %zu  exports %zu\n", n.syms.size(), n.imports().size(),
                   n.exports().size());
            std::map<uint32_t, int> rc;
            for (auto &r : n.relocs) rc[r.type]++;
            printf("  relocs");
            for (auto &kv : rc) printf(" %u:%d", kv.first, kv.second);
            printf("\n");
        } catch (const std::exception &e) {
            printf("ERROR %s: %s\n", argv[i], e.what());
            return 1;
        }
    }
    return 0;
}
