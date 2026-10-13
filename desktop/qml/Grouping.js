// Model family grouping, ported from web render.ts groups()/KNOWN.
.pragma library

var KNOWN = [
    ["Real-ESRGANv3", "Real-ESRGAN v3"],
    ["Real-ESRGAN-x4plus", "Real-ESRGAN x4plus"],
    ["Real-CUGAN-pro", "Real-CUGAN pro"],
    ["Real-CUGAN-se", "Real-CUGAN se"],
];

function groups(models) {
    var sorted = models.slice().sort(function (a, b) { return a.name < b.name ? -1 : a.name > b.name ? 1 : 0 });
    var used = {};
    var out = [];
    KNOWN.forEach(function (pair) {
        var prefix = pair[0], label = pair[1];
        var members = sorted.filter(function (m) { return m.name === prefix || m.name.indexOf(prefix + "-") === 0 });
        if (!members.length) return;
        members.forEach(function (m) { used[m.name] = true });
        // A lone member gains nothing from a family header (the row carries
        // the full name); labeling it made unrelated unlabeled rows below
        // read as part of the family.
        if (members.length === 1) {
            out.push({ label: "", items: [{ m: members[0], short: members[0].name }] });
            return;
        }
        var items = members.map(function (m) {
            return { m: m, short: m.name === prefix ? m.name : m.name.slice(prefix.length + 1) };
        });
        out.push({ label: label, items: items });
    });
    var rest = {};
    var restOrder = [];
    sorted.forEach(function (m) {
        if (used[m.name]) return;
        var stem = m.name.replace(/-x\d+.*$/i, "");
        if (!rest[stem]) { rest[stem] = []; restOrder.push(stem); }
        rest[stem].push(m);
    });
    restOrder.forEach(function (stem) {
        var items = rest[stem];
        // A stem with one member has nothing to group: the family label and
        // the row's `xN` suffix both repeat the same name ("AnimeSharp" over
        // "x4"). Emit it as a single row carrying the full model name.
        if (items.length === 1) {
            out.push({ label: "", items: [{ m: items[0], short: items[0].name }] });
            return;
        }
        out.push({
            label: stem,
            items: items.map(function (m) {
                return {
                    m: m,
                    short: m.name === stem ? m.name : m.name.slice(stem.length).replace(/^-/, ""),
                };
            }),
        });
    });
    return out;
}

function groupFP(fp) {
    return fp ? fp.replace(/(.{4})/g, "$1 ").trim() : "";
}

function shortVer(v) {
    if (!v) return "";
    var m = v.match(/(\d+\.\d+\.\d+)/);
    return m ? m[1] : v.slice(0, 12);
}
