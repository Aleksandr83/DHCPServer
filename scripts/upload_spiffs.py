Import("env")
import sys
import subprocess

def upload_spiffs(source, target, env):
    print("Uploading SPIFFS filesystem...")
    # Get the current build environment name (e.g. "esp32dev-debug")
    env_name = env.subst("$PIOENV")
    try:
        subprocess.check_call([
            sys.executable, "-m", "platformio",
            "run", "-e", env_name, "--target", "uploadfs"
        ])
        print("SPIFFS upload complete!")
    except subprocess.CalledProcessError as e:
        print("SPIFFS upload failed: %s" % str(e))
        raise

env.AddPostAction("upload", upload_spiffs)


