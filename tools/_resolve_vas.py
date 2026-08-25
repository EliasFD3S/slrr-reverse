import json
from pathlib import Path

reg = json.loads(
    Path(r"c:\Users\Elias\Desktop\re-engineering\native\docs\native_registry.json").read_text(
        encoding="utf-8"
    )
)
want = [
    ("java.game.parts.WheelRef", "getHBrake", None),
    ("java.game.parts.WheelRef", "setOppWheel", None),
    ("java.game.parts.WheelRef", "setHBrake", None),
    ("java.game.parts.WheelRef", "setArm", None),
    ("java.game.parts.WheelRef", "getPos", None),
    ("java.game.parts.bodypart.Chassis", "getWheel", None),
    ("java.game.parts.bodypart.Chassis", "getTorque", "(FF)F"),
    ("java.game.parts.bodypart.Chassis", "getWheelDamage", None),
    ("java.game.parts.bodypart.Chassis", "getWheels", None),
    ("java.game.parts.Part", "isSlotDisabled", None),
    ("java.game.parts.Part", "setSfxLoopParams", None),
    ("java.game.parts.Part", "disableSlot", None),
    ("java.game.parts.Part", "getWheelID", None),
    ("java.game.parts.Part", "getMass", None),
    ("java.util.resource.GroundRef", "removePedestrianType", None),
    ("java.util.resource.GroundRef", "getRouteLength", "()F"),
    ("java.util.resource.GroundRef", "getNearestCross", None),
    ("java.util.resource.GroundRef", "alignToRoad", None),
    ("java.util.resource.GroundRef", "findRoute", None),
    ("java.util.resource.GroundRef", "getStartDirection", None),
    ("java.render.GfxEngine", "setGlobalEnvmap", None),
    ("java.render.GfxEngine", "forceRendering", None),
    ("java.util.resource.ParticleSystem", "setFreq", None),
    ("java.util.resource.ParticleSystem", "setCounter", None),
    ("java.util.resource.ParticleSystem", "modePermanent", None),
    ("java.util.resource.RenderRef", "getDetail", None),
    ("java.util.resource.RenderRef", "addPoints", None),
    ("java.util.resource.RenderRef", "weld", None),
    ("java.util.resource.PhysicsRef", "createBox", None),
    ("java.util.resource.Animation", "init", None),
    ("java.lang.System", "isLoading", None),
    ("java.lang.System", "setLdPriority", None),
    ("java.lang.System", "isLoadingReset", None),
    ("java.lang.System", "openLib", None),
    ("java.lang.System", "getConfigOptions", None),
    ("java.io.Controller", "user_Reset", None),
    ("java.io.Controller", "user_Del", None),
    ("java.io.Controller", "user_Add", None),
    ("java.io.Controller", "user_GetAxisVal", None),
    ("java.io.Controller", "user_SetAxisSpeed", None),
    ("java.io.Controller", "user_SetAxisSmooth", None),
    ("java.io.Input", "getAxis", None),
    ("java.io.Input", "mapAxis", None),
    ("java.io.Input", "getDeviceName", None),
    ("java.io.Input", "setAxisSmooth", None),
    ("java.io.Input", "flushHotkeys", None),
    ("java.lang.Object", "hashCode", None),
    ("java.lang.Object", "toString", None),
    ("java.lang.GameType", "dispatchCursor", None),
    ("java.lang.GameType", "dispatchCursorTo", None),
    ("java.lang.Vector3", "rotate", None),
    ("java.util.resource.GameRef", "create", None),
    ("java.util.resource.GameRef", "getInfo", None),
    ("java.render.Camera", "deactivate", None),
    ("java.io.MouseCursor", "tickSysCursor", None),
]
for cls, name, sig in want:
    for e in reg["entries"]:
        if e["class"] != cls or e["name"] != name:
            continue
        if sig and e["signature"] != sig:
            continue
        print(f"{e['class']}.{e['name']}{e['signature']}\t{e['impl_va']}")
