/* ============================================================
   MONATOMIC MODULE REGISTRY — core/registry.js        v1.0.0
   ------------------------------------------------------------
   Every UI subsystem registers here as an independent block:

     MN.define("name", "1.2.0", ["dep-a", "dep-b"], (deps) => api)

   Contracts:
     - A module touches other modules ONLY through the api object
       returned by its dependencies (never their internals).
     - Bump the version on every behavioral change; MODULES.md is
       the authoritative changelog (update it in the same commit).
     - A module must tolerate a missing OPTIONAL dependency
       (MN.get returns null) — degrade, don't throw.

   Introspection:
     MN.topology()  -> { nodes:[{name,version}], edges:[[from,to]] }
     MN.report()    -> console table + mermaid graph text (paste
                       into MODULES.md when the graph changes)
   ============================================================ */
window.MN = (function () {
  "use strict";

  const mods = {};      /* name -> {version, deps, factory, api, ready} */
  const order = [];     /* definition order, for boot */

  function define(name, version, deps, factory) {
    if (mods[name]) {
      console.warn("[MN] duplicate module definition ignored:", name);
      return;
    }
    mods[name] = { version, deps: deps || [], factory, api: null, ready: false };
    order.push(name);
  }

  /* Resolve a module, initializing it (and its deps) on first use. */
  function get(name) {
    const m = mods[name];
    if (!m) return null;
    if (m.ready) return m.api;
    if (m._resolving) {
      console.error("[MN] circular dependency at", name);
      return null;
    }
    m._resolving = true;
    const resolved = {};
    m.deps.forEach((d) => { resolved[d] = get(d); });
    try {
      m.api = m.factory(resolved) || {};
    } catch (e) {
      console.error("[MN] module init failed:", name, e);
      m.api = {};
    }
    m._resolving = false;
    m.ready = true;
    return m.api;
  }

  /* Initialize every defined module (boot order = definition order). */
  function boot() {
    order.forEach(get);
  }

  function topology() {
    const nodes = order.map((n) => ({ name: n, version: mods[n].version }));
    const edges = [];
    order.forEach((n) => mods[n].deps.forEach((d) => edges.push([n, d])));
    return { nodes, edges };
  }

  function report() {
    const t = topology();
    try { console.table(t.nodes); } catch (_) {}
    let mermaid = "graph TD\n";
    t.nodes.forEach((n) => {
      mermaid += `  ${n.name.replace(/[^a-z0-9]/gi, "_")}["${n.name} v${n.version}"]\n`;
    });
    t.edges.forEach(([a, b]) => {
      mermaid += `  ${a.replace(/[^a-z0-9]/gi, "_")} --> ${b.replace(/[^a-z0-9]/gi, "_")}\n`;
    });
    console.log(mermaid);
    return mermaid;
  }

  return { define, get, boot, topology, report };
})();
