
#include "vox_ini.h"
#include <stdio.h>
#include <string.h>

int main() {
    printf("=== vox_ini 示例 ===\n\n");

    vox_mpool_t* mpool = vox_mpool_create();
    const char* ini_content = 
        "; 这是一个配置文件\n"
        "[owner]\n"
        "name=John Doe\n"
        "organization=Acme Widgets Inc.\n"
        "\n"
        "[database]\n"
        "server=192.0.2.62\n"
        "port=143\n"
        "file=\"payroll.dat\"\n";

    printf("--- 1. 解析 ---\n");
    vox_ini_t* ini = vox_ini_parse(mpool, ini_content, NULL);
    if (ini) {
        printf("解析成功。\n");
        printf("Owner Name: %s\n", vox_ini_get_value(ini, "owner", "name"));
        printf("DB Server: %s\n", vox_ini_get_value(ini, "database", "server"));
        printf("DB Port: %s\n", vox_ini_get_value(ini, "database", "port"));
    }

    printf("\n--- 2. 修改与写入 ---\n");
    vox_ini_set_value(ini, "database", "port", "5432");
    vox_ini_set_value(ini, "database", "user", "admin");
    vox_ini_set_value(ini, "network", "proxy", "http://proxy.example.com");

    size_t out_size;
    char* new_content = vox_ini_to_string(ini, &out_size);
    if (new_content) {
        printf("新内容:\n%s", new_content);
    }

    printf("\n--- 3. 删除 ---\n");
    vox_ini_remove_key(ini, "owner", "organization");
    vox_ini_remove_section(ini, "network");
    
    new_content = vox_ini_to_string(ini, &out_size);
    printf("删除后的内容:\n%s", new_content);

    printf("\n--- 4. 文件操作 ---\n");
    const char* test_file = "test.ini";
    if (vox_ini_write_file(ini, test_file) == 0) {
        printf("成功写入文件: %s\n", test_file);
        
        vox_ini_t* ini2 = vox_ini_parse_file(mpool, test_file, NULL);
        if (ini2) {
            printf("成功从文件解析: %s\n", test_file);
            printf("Owner Name (from file): %s\n", vox_ini_get_value(ini2, "owner", "name"));
        }
    }

    vox_mpool_destroy(mpool);
    return 0;
}
