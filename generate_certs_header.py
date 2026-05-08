Import("env")
import os

certs_dir = os.path.join(env.subst("$PROJECT_SRC_DIR"), "certs")
header_path = os.path.join(env.subst("$PROJECT_SRC_DIR"), "certs.h")

cert_files = ["AmazonRootCA1.pem", "device.cert.pem", "private.key.pem"]

lines = ["#ifndef CERTS_H", "#define CERTS_H", ""]

for cert_file in cert_files:
    cert_path = os.path.join(certs_dir, cert_file)
    var_name = cert_file.replace(".", "_").replace("-", "_")
    with open(cert_path, "r") as f:
        content = f.read()
    lines.append(f'static const char* {var_name} = R"__CERT__({content})__CERT__";')
    lines.append("")

lines.append("#endif")

with open(header_path, "w") as f:
    f.write("\n".join(lines) + "\n")

print(f"Generated {header_path}")
