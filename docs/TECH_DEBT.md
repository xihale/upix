# 技术债与简化待办

> 来源：原过度工程审计（`docs/PONYTAIL_AUDIT.md`，已归档于 git 历史）。
> 26 项中 22 项已闭环删除，以下为经核实仍存在、值得后续处理的遗留项。
> 按预计收益排序。

---

### B. `TileSizeOptimizer` 投机部分 [yagni] ~省 100 行

- `share/core/tile_optimizer.hpp:131` 的 `estimate_tile_size`（0 调用，被
  `find_optimal_tile_size` 完全绕过）
- `config.hpp` 的 `DevicePerformanceProfile` / `ModelConstraints` 投机字段

**建议**：删 `estimate_tile_size`；`DevicePerformanceProfile` 砍到只剩
`available_memory_mb`。保留 `find_optimal_tile_size`（实测选优，合理）。

### C. `worker.cpp` `connect()` 构造未发出的 `auth_req` [delete] ~省 5 行

`share/core/distributed/worker.cpp:74-75`：构造 `AuthRequest auth_req`
但没有发出（真正发送在 `handshake()` :99-105 重新构造一遍）。删死构造即可。

### D. Navigation ~42 个回调透传 [yagni/shrink] ~省 50 行

`apps/app/src/main/java/com/crossscale/ui/navigation/Navigation.kt:36-77`：
约 42 个 `onXxx` 回调，绝大多数是 `mainViewModel::xxx` 直接透传给 HomeScreen。

**建议**：给 HomeScreen 提取 `HomeCallbacks` 数据类，或让 HomeScreen 内部
`viewModel()` 取 VM 绕过透传。需改 HomeScreen 签名，工作量中等。

### E. `ProcessingViewModel` failureNames 三函数合并 [shrink] ~省 15 行

`ProcessingViewModel.kt:1750,1758`：`orderedFailureDetails` /
`failureNamesFromOrdered` 被多处调用、功能正确，但可合并为一个：

```kotlin
fun failureNames(m) = m.values.sortedBy { ... }.map { it.displayName }
```
