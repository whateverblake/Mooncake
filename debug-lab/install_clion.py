#!/usr/bin/env python3
"""Install the lab's local CLion 2026.1 settings while CLion is closed.

Preserves existing SSH connections/toolchains and backs up modified settings.
The files contain a private-key path, never the key contents.
"""

import pathlib
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
OPTIONS = pathlib.Path.home() / "Library/Application Support/JetBrains/CLion2026.1/options"
BACKUP = ROOT / "debug-lab/.local/clion-backup"
NAME = "Mooncake Docker"
SSH_ID = "72bb1c0c-147f-46bf-a675-9349b18c35d8"
SERVER_ID = "2232a62e-57f9-44f9-9879-b4052616bb57"


def load(relative):
    path = OPTIONS / relative
    if path.exists():
        backup = BACKUP / relative
        if not backup.exists():
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, backup)
        return ET.parse(path)
    return ET.ElementTree(ET.Element("application"))


def child(parent, tag, **attrs):
    for element in parent.findall(tag):
        if all(element.get(key) == value for key, value in attrs.items()):
            return element
    return ET.SubElement(parent, tag, attrs)


def save(tree, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.indent(tree, space="  ")
    tree.write(path, encoding="unicode")
    with path.open("a") as output:
        output.write("\n")


def main():
    processes = subprocess.check_output(["ps", "ax", "-o", "comm="], text=True)
    if "/CLion.app/Contents/MacOS/clion" in processes:
        raise SystemExit("Close CLion before installing its settings.")
    if not OPTIONS.is_dir():
        raise SystemExit(f"CLion 2026.1 options not found: {OPTIONS}")
    if not (ROOT / "debug-lab/.local/id_ed25519").is_file():
        raise SystemExit("Run debug-lab/lab.sh up first to create the SSH key.")

    tree = load("sshConfigs.xml")
    configs = child(child(tree.getroot(), "component", name="SshConfigs"), "configs")
    config = child(configs, "sshConfig", id=SSH_ID)
    config.attrib.update(
        authType="KEY_PAIR", host="127.0.0.1", port="33333", username="debugger",
        keyPath=str(ROOT / "debug-lab/.local/id_ed25519"),
        nameFormat="CUSTOM", customName=NAME, useOpenSSHConfig="false",
    )
    save(tree, OPTIONS / "sshConfigs.xml")

    tree = load("webServers.xml")
    component = child(tree.getroot(), "component", name="WebServers")
    server = child(child(component, "option", name="servers"), "webServer", id=SERVER_ID)
    server.attrib.update(name=NAME, url="http://127.0.0.1")
    transfer = child(server, "fileTransfer")
    transfer.attrib.update(
        accessType="MOUNT", host="127.0.0.1", port="33333",
        rootFolder="/workspace/mooncake", mountedRoot=str(ROOT),
        sshConfigId=SSH_ID, sshConfig=NAME,
    )
    save(tree, OPTIONS / "webServers.xml")

    tree = load("mac/toolchains.xml")
    component = child(tree.getroot(), "component", name="CPPToolchains")
    component.set("version", "10")
    toolchain = child(child(component, "toolchains"), "toolchain", name=NAME)
    toolchain.attrib.update(
        toolSetKind="SSH", hostId=SERVER_ID, sshConfigName=NAME,
        customCMakePath="/usr/bin/cmake", customMakePath="/usr/bin/ninja",
        customCCompilerPath="/usr/bin/gcc", customCXXCompilerPath="/usr/bin/g++",
        debuggerKind="CUSTOM_GDB", customGDBPath="/usr/bin/gdb",
        environment="/workspace/mooncake/debug-lab/env.sh",
    )
    save(tree, OPTIONS / "mac/toolchains.xml")

    # This fresh project's configuration uses the existing container build.
    options = " ".join(re.findall(r"-D[^\s\\]+", (ROOT / "debug-lab/configure.sh").read_text()))
    project = ET.Element("project", version="4")
    component = child(project, "component", name="CMakeSharedSettings")
    child(child(component, "configurations"), "configuration",
          PROFILE_NAME="Mooncake Debug", ENABLED="true", CONFIG_NAME="Debug",
          TOOLCHAIN_NAME=NAME, GENERATION_DIR="/workspace/build",
          GENERATION_OPTIONS="-G Ninja " + options, BUILD_OPTIONS="-j 2")
    save(ET.ElementTree(project), ROOT / ".idea/cmake.xml")

    project = ET.Element("project", version="4")
    component = child(project, "component", name="PublishConfigData")
    component.set("serverName", NAME)
    paths = child(child(child(component, "serverData"), "paths", name=NAME), "serverdata")
    child(child(paths, "mappings"), "mapping", deploy="/",
          local="$PROJECT_DIR$", web="/")
    excludes = child(paths, "excludedPaths")
    for directory in (".git", "debug-lab/.local", ".idea"):
        child(excludes, "excludedPath", local="true", path=f"$PROJECT_DIR$/{directory}")
    save(ET.ElementTree(project), ROOT / ".idea/deployment.xml")

    for role in ("roundtrip", "owner"):
        project = ET.Element("component", name="ProjectRunConfigurationManager")
        config = child(project, "configuration", default="false",
                       name=f"Mooncake {role}", type="CMakeRunConfiguration",
                       factoryName="Application", PROGRAM_PARAMS=role,
                       PASS_PARENT_ENVS_2="true", PROJECT_NAME="mooncake",
                       TARGET_NAME="store_debug_demo", CONFIG_NAME="Mooncake Debug",
                       RUN_TARGET_PROJECT_NAME="mooncake", RUN_TARGET_NAME="store_debug_demo")
        child(child(config, "method", v="2"), "option",
              name="com.jetbrains.cidr.execution.CidrBuildBeforeRunTaskProvider$BuildBeforeRunTask",
              enabled="true")
        save(ET.ElementTree(project), ROOT / f".idea/runConfigurations/Mooncake_{role}.xml")
    print(f"Installed {NAME} toolchain, SSH connection, source mapping, and run configurations.")
    print(f"Previous application settings backed up in {BACKUP}")


if __name__ == "__main__":
    main()
