Import("env")

import os
import shutil
import subprocess


def generate_insights_package(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")

    firmware = os.path.join(build_dir, "firmware.bin")
    elf = os.path.join(build_dir, "firmware.elf")
    map_file = os.path.join(build_dir, "firmware.map")
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions_bin = os.path.join(build_dir, "partitions.bin")
    partitions_csv = os.path.join(project_dir, "partitions_ota.csv")

    required = [
        firmware,
        elf,
        map_file,
        bootloader,
        partitions_bin,
        partitions_csv,
    ]

    missing = [p for p in required if not os.path.isfile(p)]

    if missing:
        print("ESP Insights: build artifacts are not available yet; skipping package.")
        return

    # Espressif's Arduino generator expects these names.
    generated_bootloader = os.path.join(build_dir, "firmware.bootloader.bin")
    generated_partitions = os.path.join(build_dir, "firmware.partitions.bin")
    generated_csv = os.path.join(build_dir, "partitions.csv")

    shutil.copy2(bootloader, generated_bootloader)
    shutil.copy2(partitions_bin, generated_partitions)
    shutil.copy2(partitions_csv, generated_csv)

    generator = os.path.expanduser(
        "~/.platformio/packages/"
        "framework-arduinoespressif32@src-be081158b8eddfb860d06ee01647245d/"
        "tools/gen_insights_package.py"
    )

    if not os.path.isfile(generator):
        raise RuntimeError(
            "ESP Insights generator not found:\n" + generator
        )

    print("")
    print("Generating ESP Insights firmware package...")

    subprocess.check_call([
        env.subst("$PYTHONEXE"),
        generator,
        build_dir,
        "firmware",
        project_dir,
    ])

    print("")
    print("ESP Insights package created:")
    print(os.path.join(project_dir, "firmware.zip"))


# This is deliberately a separate target rather than a post-build hook.
insights_target = env.Alias(
    "insights",
    [],
    generate_insights_package,
)

AlwaysBuild(insights_target)