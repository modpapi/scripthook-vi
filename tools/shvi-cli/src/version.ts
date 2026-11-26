import pkg from "../package.json";

// The CLI and the SDK are released together: shvi <x.y.z> creates mods against SDK <x.y.z>.
export const VERSION: string = pkg.version;
