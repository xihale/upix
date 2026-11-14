//! Model catalog, family grouping, scene metadata classification, and JSON
//! exports matching `upix-core` (`model_catalog`, `model_families`,
//! `model_metadata`, `model_graph::scale_from_filename`) and `upix-wasm::codec`.

const std = @import("std");

pub const CATALOG = [_][]const u8{
    "AnimeJaNai-v2-Compact-x2",
    "AnimeSharp-x4",
    "Archivist-Medium-Compact-x1",
    "Archivist-Rough-Compact-x1",
    "BSRGAN-x2",
    "BSRGAN-x4",
    "DeH264-RealPLKSR-x1",
    "DeJPG-RealPLKSR-x1",
    "DeNoise-RealPLKSR-x1",
    "Genesis-Cleanup-Compact-x1",
    "HFA2k-LUDVAE-SPAN-x2",
    "HFA2k-x4",
    "LSDIR-Compact-v2-x4",
    "ModernSpanimation-x2",
    "NES-Composite-RGB-Compact-x1",
    "NMKD-Siax-x4",
    "Nomos2-OTF-x4",
    "Nomos8kSC-x4",
    "NomosUni-Compact-x2",
    "NomosUni-SPAN-x2",
    "NomosUni-SPAN-x4",
    "NomosWebPhoto-RealPLKSR-x4",
    "Parimg-Compact-x2",
    "Real-CUGAN-denoise-x2",
    "Real-CUGAN-denoise-x3",
    "Real-CUGAN-no-denoise-x2",
    "Real-ESRGAN-x2plus",
    "Real-ESRGAN-x4plus",
    "Real-ESRGAN-x4plus-anime",
    "Real-ESRGANv3-anime-x2",
    "Real-ESRGANv3-anime-x3",
    "Real-ESRGANv3-general-x4",
    "Remacri-x4",
    "UltraSharp-x4",
};

pub const KnownFamily = struct {
    prefix: []const u8,
    label: []const u8,
};

pub const KNOWN_FAMILIES = [_]KnownFamily{
    .{ .prefix = "Real-ESRGANv3", .label = "Real-ESRGAN v3" },
    .{ .prefix = "Real-ESRGAN-x4plus", .label = "Real-ESRGAN x4plus" },
    .{ .prefix = "Real-CUGAN-pro", .label = "Real-CUGAN pro" },
    .{ .prefix = "Real-CUGAN-se", .label = "Real-CUGAN se" },
};

pub const SceneRule = struct {
    pattern: []const u8,
    scene: []const u8,
    recommended: bool,
};

pub const SCENE_RULES = [_]SceneRule{
    .{ .pattern = "real-esrganv3-anime", .scene = "动漫", .recommended = true },
    .{ .pattern = "real-esrganv3-general", .scene = "照片", .recommended = true },
    .{ .pattern = "real-esrgan-x4plus-anime", .scene = "动漫", .recommended = false },
    .{ .pattern = "real-esrgan-x4plus", .scene = "照片", .recommended = true },
    .{ .pattern = "real-esrgan-x2plus", .scene = "照片", .recommended = false },
    .{ .pattern = "real-cugan", .scene = "动漫", .recommended = false },
    .{ .pattern = "nomosuni-compact", .scene = "照片", .recommended = false },
    .{ .pattern = "parimg-compact", .scene = "照片", .recommended = false },
    .{ .pattern = "nomosuni-span", .scene = "照片", .recommended = false },
    .{ .pattern = "lsdir-compact", .scene = "照片", .recommended = false },
    .{ .pattern = "nomos2-otf", .scene = "照片", .recommended = false },
    .{ .pattern = "nomos8ksc", .scene = "照片", .recommended = false },
    .{ .pattern = "nomoswebphoto", .scene = "照片", .recommended = false },
    .{ .pattern = "dejpg", .scene = "照片·修复", .recommended = false },
    .{ .pattern = "denoise", .scene = "照片·修复", .recommended = false },
    .{ .pattern = "deh264", .scene = "视频·修复", .recommended = false },
    .{ .pattern = "archivist-medium", .scene = "动漫·修复", .recommended = true },
    .{ .pattern = "archivist-rough", .scene = "动漫·修复", .recommended = false },
    .{ .pattern = "nes-composite", .scene = "游戏·修复", .recommended = false },
    .{ .pattern = "genesis-cleanup", .scene = "游戏·修复", .recommended = false },
    .{ .pattern = "hfa2k", .scene = "动漫", .recommended = false },
    .{ .pattern = "modernspanimation", .scene = "动漫", .recommended = false },
    .{ .pattern = "ultrasharp", .scene = "综合", .recommended = true },
    .{ .pattern = "remacri", .scene = "综合", .recommended = true },
    .{ .pattern = "animesharp", .scene = "动漫", .recommended = true },
    .{ .pattern = "animejanai", .scene = "动漫", .recommended = false },
    .{ .pattern = "nmkd-siax", .scene = "综合", .recommended = false },
    .{ .pattern = "bsrgan", .scene = "照片", .recommended = false },
};

pub const ModelSceneInfo = struct {
    scene: []const u8 = "",
    recommended: bool = false,
};

pub const CatalogModelEntry = struct {
    name: []const u8,
    short_name: []const u8,
    family: []const u8,
    scale: i32,
    scene: []const u8,
    recommended: bool,
};

pub fn names() []const []const u8 {
    return &CATALOG;
}

pub fn isKnown(name: []const u8) bool {
    for (CATALOG) |entry| {
        if (std.mem.eql(u8, entry, name)) return true;
    }
    return false;
}

/// Extract the LAST "x<digits>" run whose value lands in 1..8.
pub fn scaleFromFilename(name: []const u8) ?i32 {
    if (name.len < 2) return null;
    var from_name: i32 = 0;
    var i: usize = 0;
    while (i + 1 < name.len) : (i += 1) {
        if (name[i] != 'x' and name[i] != 'X') continue;
        if (!std.ascii.isDigit(name[i + 1])) continue;
        var n: i32 = 0;
        var j = i + 1;
        while (j < name.len and n <= 8 and std.ascii.isDigit(name[j])) : (j += 1) {
            n = n * 10 + @as(i32, name[j] - '0');
        }
        if (n >= 1 and n <= 8) {
            from_name = n;
        }
    }
    return if (from_name >= 1) from_name else null;
}

/// Stem = name minus a case-insensitive trailing `-x<N>...`.
pub fn modelStem(name: []const u8) []const u8 {
    if (name.len < 3) return name;
    var i: usize = 0;
    while (i + 2 < name.len) : (i += 1) {
        if (name[i] == '-' and (name[i + 1] == 'x' or name[i + 1] == 'X') and std.ascii.isDigit(name[i + 2])) {
            return name[0..i];
        }
    }
    return name;
}

/// Short display name of a model (strips known family prefix and leading dash).
pub fn shortModelName(name: []const u8) []const u8 {
    for (KNOWN_FAMILIES) |fam| {
        if (std.mem.startsWith(u8, name, fam.prefix) and name.len > fam.prefix.len + 1 and name[fam.prefix.len] == '-') {
            return name[fam.prefix.len + 1 ..];
        }
    }
    return name;
}

fn normalizeByte(b: u8) u8 {
    if (b == '_' or b == ' ') return '-';
    return std.ascii.toLower(b);
}

fn containsNormalized(haystack: []const u8, pattern: []const u8) bool {
    if (pattern.len == 0) return true;
    if (haystack.len < pattern.len) return false;
    const limit = haystack.len - pattern.len + 1;
    for (0..limit) |start| {
        var matched = true;
        for (0..pattern.len) |k| {
            if (normalizeByte(haystack[start + k]) != pattern[k]) {
                matched = false;
                break;
            }
        }
        if (matched) return true;
    }
    return false;
}

/// Classify a model name into its scene badge. Unknown -> empty info.
pub fn classifyModelScene(name: []const u8) ModelSceneInfo {
    for (SCENE_RULES) |rule| {
        if (containsNormalized(name, rule.pattern)) {
            return .{ .scene = rule.scene, .recommended = rule.recommended };
        }
    }
    return .{};
}

const FamilyMeta = struct {
    family: []const u8,
    short_name: []const u8,
};

fn computeCatalogFamilies(allocator: std.mem.Allocator) ![]FamilyMeta {
    const n = CATALOG.len;
    const out = try allocator.alloc(FamilyMeta, n);
    var used = try allocator.alloc(bool, n);
    defer allocator.free(used);
    @memset(used, false);

    // Build sorted index list matching `model_families::groups` alphabetical sort.
    var sorted_idx = try allocator.alloc(usize, n);
    defer allocator.free(sorted_idx);
    for (0..n) |i| sorted_idx[i] = i;
    std.mem.sort(usize, sorted_idx, {}, struct {
        fn lessThan(_: void, a: usize, b: usize) bool {
            return std.mem.order(u8, CATALOG[a], CATALOG[b]) == .lt;
        }
    }.lessThan);

    // 1. Known families
    var members: std.ArrayList(usize) = .empty;
    defer members.deinit(allocator);
    for (KNOWN_FAMILIES) |fam| {
        members.clearRetainingCapacity();
        for (sorted_idx) |orig_i| {
            const name = CATALOG[orig_i];
            if (std.mem.eql(u8, name, fam.prefix) or
                (std.mem.startsWith(u8, name, fam.prefix) and name.len > fam.prefix.len and name[fam.prefix.len] == '-'))
            {
                try members.append(allocator, orig_i);
            }
        }
        if (members.items.len == 0) continue;
        for (members.items) |orig_i| used[orig_i] = true;
        if (members.items.len == 1) {
            const orig_i = members.items[0];
            out[orig_i] = .{ .family = "", .short_name = CATALOG[orig_i] };
            continue;
        }
        for (members.items) |orig_i| {
            const name = CATALOG[orig_i];
            const sn = if (std.mem.eql(u8, name, fam.prefix)) name else name[fam.prefix.len + 1 ..];
            out[orig_i] = .{ .family = fam.label, .short_name = sn };
        }
    }

    // 2. Remaining entries grouped by stem
    var stems: std.ArrayList([]const u8) = .empty;
    defer stems.deinit(allocator);
    var by_stem: std.ArrayList(std.ArrayList(usize)) = .empty;
    defer {
        for (by_stem.items) |*list| list.deinit(allocator);
        by_stem.deinit(allocator);
    }

    for (sorted_idx) |orig_i| {
        if (used[orig_i]) continue;
        const stem = modelStem(CATALOG[orig_i]);
        var found: ?usize = null;
        for (stems.items, 0..) |s, si| {
            if (std.mem.eql(u8, s, stem)) {
                found = si;
                break;
            }
        }
        if (found) |si| {
            try by_stem.items[si].append(allocator, orig_i);
        } else {
            try stems.append(allocator, stem);
            var list: std.ArrayList(usize) = .empty;
            try list.append(allocator, orig_i);
            try by_stem.append(allocator, list);
        }
    }

    for (stems.items, by_stem.items) |stem, list| {
        if (list.items.len == 1) {
            const orig_i = list.items[0];
            out[orig_i] = .{ .family = "", .short_name = CATALOG[orig_i] };
            continue;
        }
        for (list.items) |orig_i| {
            const name = CATALOG[orig_i];
            var sn = if (std.mem.eql(u8, name, stem)) name else name[stem.len..];
            if (sn.len > 0 and sn[0] == '-') sn = sn[1..];
            out[orig_i] = .{ .family = stem, .short_name = sn };
        }
    }

    return out;
}

fn writeEntryJson(ws: *std.json.Stringify, entry: CatalogModelEntry) !void {
    try ws.beginObject();
    try ws.objectField("name");
    try ws.write(entry.name);
    try ws.objectField("short_name");
    try ws.write(entry.short_name);
    try ws.objectField("family");
    try ws.write(entry.family);
    try ws.objectField("scale");
    try ws.write(entry.scale);
    try ws.objectField("scene");
    try ws.write(entry.scene);
    try ws.objectField("recommended");
    try ws.write(entry.recommended);
    try ws.endObject();
}

/// Export the full model catalog as a JSON array matching `upix_catalog_json`.
/// Caller owns the returned slice.
pub fn exportCatalogJson(allocator: std.mem.Allocator) ![]u8 {
    const families = try computeCatalogFamilies(allocator);
    defer allocator.free(families);

    var out: std.Io.Writer.Allocating = .init(allocator);
    errdefer out.deinit();
    var ws: std.json.Stringify = .{ .writer = &out.writer };

    try ws.beginArray();
    for (CATALOG, 0..) |name, i| {
        const meta = classifyModelScene(name);
        const scale = scaleFromFilename(name) orelse 2;
        try writeEntryJson(&ws, .{
            .name = name,
            .short_name = families[i].short_name,
            .family = families[i].family,
            .scale = scale,
            .scene = meta.scene,
            .recommended = meta.recommended,
        });
    }
    try ws.endArray();
    return out.toOwnedSlice();
}

/// Classify a single model name and return JSON matching `upix_classify_model_json`.
/// Caller owns the returned slice.
pub fn classifyModelJson(allocator: std.mem.Allocator, name: []const u8) ![]u8 {
    const meta = classifyModelScene(name);
    const scale = scaleFromFilename(name) orelse 2;

    var out: std.Io.Writer.Allocating = .init(allocator);
    errdefer out.deinit();
    var ws: std.json.Stringify = .{ .writer = &out.writer };

    try writeEntryJson(&ws, .{
        .name = name,
        .short_name = shortModelName(name),
        .family = modelStem(name),
        .scale = scale,
        .scene = meta.scene,
        .recommended = meta.recommended,
    });
    return out.toOwnedSlice();
}

test "catalog entries are valid and scene-classifiable" {
    const testing = std.testing;
    for (CATALOG) |name| {
        const meta = classifyModelScene(name);
        try testing.expect(meta.scene.len > 0);
        try testing.expect(scaleFromFilename(name) != null);
    }
    try testing.expect(isKnown("AnimeJaNai-v2-Compact-x2"));
    try testing.expect(!isKnown("Animesharp-x4"));
}

test "scaleFromFilename matches Rust cases" {
    const testing = std.testing;
    try testing.expectEqual(@as(?i32, 2), scaleFromFilename("Real-ESRGANv3-anime-x2"));
    try testing.expectEqual(@as(?i32, 4), scaleFromFilename("Real-ESRGAN-x4plus"));
    try testing.expectEqual(@as(?i32, 4), scaleFromFilename("x4"));
    try testing.expectEqual(@as(?i32, 3), scaleFromFilename("X3net"));
    try testing.expectEqual(@as(?i32, 4), scaleFromFilename("x2-then-x4"));
    try testing.expectEqual(@as(?i32, 1), scaleFromFilename("net-x2plus-x1"));
    try testing.expectEqual(@as(?i32, null), scaleFromFilename("x9"));
    try testing.expectEqual(@as(?i32, null), scaleFromFilename("x16"));
    try testing.expectEqual(@as(?i32, null), scaleFromFilename("x99999999999"));
    try testing.expectEqual(@as(?i32, null), scaleFromFilename("plain"));
    try testing.expectEqual(@as(?i32, null), scaleFromFilename("x"));
}

test "classifyModelScene specificity and normalization" {
    const testing = std.testing;
    const anime = classifyModelScene("Real-ESRGANv3-anime-x2");
    try testing.expectEqualStrings("动漫", anime.scene);
    try testing.expect(anime.recommended);

    try testing.expectEqualStrings("动漫", classifyModelScene("real-esrgan-x4plus-anime").scene);
    try testing.expectEqualStrings("照片", classifyModelScene("real-esrgan-x4plus").scene);
    try testing.expectEqualStrings("动漫", classifyModelScene("real-cugan-denoise-x2").scene);
    try testing.expectEqualStrings("动漫", classifyModelScene("Animesharp x4").scene);
    try testing.expectEqualStrings("动漫", classifyModelScene("Real_ESRGANv3_anime_x2").scene);
    try testing.expectEqualStrings("", classifyModelScene("totally-custom-net").scene);
    try testing.expect(!classifyModelScene("totally-custom-net").recommended);
}

test "exportCatalogJson and classifyModelJson produce valid JSON" {
    const testing = std.testing;
    const cat_json = try exportCatalogJson(testing.allocator);
    defer testing.allocator.free(cat_json);

    var parsed = try std.json.parseFromSlice([]CatalogModelEntry, testing.allocator, cat_json, .{});
    defer parsed.deinit();
    try testing.expectEqual(CATALOG.len, parsed.value.len);

    // Verify Real-ESRGANv3 grouping
    var found_v3 = false;
    var found_bsrgan = false;
    for (parsed.value) |entry| {
        if (std.mem.eql(u8, entry.name, "Real-ESRGANv3-anime-x2")) {
            found_v3 = true;
            try testing.expectEqualStrings("Real-ESRGAN v3", entry.family);
            try testing.expectEqualStrings("anime-x2", entry.short_name);
            try testing.expectEqual(@as(i32, 2), entry.scale);
            try testing.expectEqualStrings("动漫", entry.scene);
            try testing.expect(entry.recommended);
        }
        if (std.mem.eql(u8, entry.name, "BSRGAN-x2")) {
            found_bsrgan = true;
            try testing.expectEqualStrings("BSRGAN", entry.family);
            try testing.expectEqualStrings("x2", entry.short_name);
        }
    }
    try testing.expect(found_v3 and found_bsrgan);

    const cls_json = try classifyModelJson(testing.allocator, "Real-ESRGANv3-anime-x2");
    defer testing.allocator.free(cls_json);
    var parsed_cls = try std.json.parseFromSlice(CatalogModelEntry, testing.allocator, cls_json, .{});
    defer parsed_cls.deinit();
    try testing.expectEqualStrings("anime-x2", parsed_cls.value.short_name);
    try testing.expectEqualStrings("Real-ESRGANv3-anime", parsed_cls.value.family);
    try testing.expectEqual(@as(i32, 2), parsed_cls.value.scale);
}
