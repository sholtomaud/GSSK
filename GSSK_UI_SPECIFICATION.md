# GSSK Diagrammatic Interface Specification (v1.0)

This document specifies the requirements and technical details for developing the **GSSK Diagram Editor**, a visual interface for building, editing, and monitoring GSSK (General Systems Simulation Kernel) models.

## 1. Overview
The GSSK Diagram Editor is a client-side, vanilla JavaScript web component (`<gssk-editor>`). It allows users to visually design system models using nodes and edges, export them as GSSK-compatible JSON, and visualize real-time simulation data from the GSSK WASM engine.

### 1.1 Core Requirements
- **No external libraries**: Implementation must use native Web APIs (SVG, Shadow DOM, Custom Elements, Pointer Events).
- **Two-way Data Binding**: Loading a JSON model renders the diagram; editing the diagram updates the JSON.
- **Odum Symbol Support**: Support both generic shapes and H.T. Odum's Energy Systems Language symbols.
- **Simulation Interfacing**: Visualize node "storage levels" and edge "flow rates" during live simulation.

---

## 2. Extended JSON Schema
The editor uses the standard GSSK JSON format but adds a `visual` property to nodes and edges to store spatial and aesthetic metadata.

### 2.1 Node Visual Metadata
```json
{
  "id": "grass",
  "type": "storage",
  "value": 10.0,
  "visual": {
    "x": 250,
    "y": 150,
    "label": "Grass Biomass",
    "capacity": 100.0,
    "symbol": "storage_tank"
  }
}
```
- `x`, `y`: Coordinates of the node center.
- `capacity`: The maximum value for visualization (used to calculate "fill" percentage).
- `symbol`: (Optional) Override for specific Odum icons.

### 2.2 Edge Visual Metadata
```json
{
  "id": "production",
  "origin": "sun",
  "target": "grass",
  "logic": "linear",
  "params": { "k": 0.1 },
  "visual": {
    "points": [[100, 50], [250, 150]],
    "max_flow": 5.0
  }
}
```
- `points`: Array of `[x, y]` waypoints for the path.
- `max_flow`: Reference flow rate for stroke-width visualization.

---

## 3. Web Component API: `<gssk-editor>`

### 3.1 Attributes & Properties
| Attribute | Property | Description |
| :--- | :--- | :--- |
| `symbols` | `symbols` | `"generic"` or `"odum"`. Defaults to `"odum"`. |
| `readonly` | `readOnly` | Boolean. If true, disables drag-and-drop and editing. |
| `grid` | `gridSize` | Number. Snapping grid size in pixels. Default `20`. |
| - | `value` | Getter/Setter for the complete GSSK JSON object. |

### 3.2 Methods
- `loadModel(json)`: Re-renders the canvas with the provided model.
- `updateState(stateArray)`: Accepts a `Float64Array` of state values. The index $i$ in the array corresponds exactly to the $i$-th element in the `nodes` array of the loaded JSON model.
- `getJson()`: Returns the current model JSON including visual metadata.

### 3.3 Events
- `change`: Dispatched whenever a node is moved, or a parameter is edited.
- `node-select`: Dispatched when a node is clicked, passing the node object in `detail`.

---

## 4. Visual Specification (SVG)

### 4.1 Node Representations (Odum Set)
- **Source**: Circle with a central dot or an Odum "cloud" symbol.
- **Storage**: A vertical "tank" shape (rectangle with rounded top/bottom).
- **Sink**: A "ground" symbol (arrow pointing into a series of horizontal lines).
- **Constant**: A simple square/diamond.

### 4.2 Edge & Interaction Logic
- **Flow Edge**: A solid line with an arrow at the target.
- **Control Edge**: For `interaction` and `limit` logic, a dashed line originates from the `control_node` and terminates at a **"Gate Symbol"** (small circle or hexagon) located on the midpoint of the flow edge.

### 4.3 Simulation Visualization
- **Node Filling**: Storage nodes should have a background "fill" color that rises/falls based on `current_value / capacity`.
- **Flow Thickness**: Flow edges should adjust their `stroke-width` dynamically based on the current flow rate calculated from the logic (or simply pulse to indicate activity).

---

## 5. Interaction Design

### 5.1 Building the Model
- **Palette**: A sidebar containing icons for Source, Storage, and Sink.
- **Drag-and-Drop**: Users drag from the palette onto the SVG canvas to create nodes.
- **Wiring**:
  - Clicking a "port" (invisible circle on node edges) and dragging creates a wire.
  - Dropping the wire onto another node creates a standard flow edge.
  - Dropping the wire onto an *existing edge* automatically sets the `control_node` for that edge and changes its logic to `interaction`.

### 5.2 Property Editing
- **Side Panel**: When a node/edge is selected, a panel appears to edit `id`, `value`, and `params.k`.
- **In-place Edit**: Double-clicking a label allows direct text editing on the canvas.

---

## 6. Implementation Guide for LLM Developer

1. **Shadow DOM**: Use a Shadow Root to encapsulate styles.
2. **SVG Viewbox**: Use an `<svg>` element with `viewBox="0 0 1000 1000"` to handle scaling and panning.
3. **State Management**:
   - Maintain an internal map of nodes and edges.
   - **Index Mapping**: Upon `loadModel`, the component must record the index of each node as they appear in the JSON array. This index is used to look up values in the `stateArray` during `updateState` calls.
   - Use `requestAnimationFrame` for smooth updates when `updateState` is called.
4. **Odum Icons**: Define icons as `<symbol>` elements in an internal SVG `<defs>` block for reusability.
5. **Path Calculation**: Use simple linear paths or Quadratic Bezier curves for edges. When a control node is linked, calculate the intersection point on the flow path to place the "gate" symbol.

## 7. Real-time Integration Example
The host application should interface with the editor as follows:
```javascript
const editor = document.querySelector('gssk-editor');

// During simulation loop
function onStep() {
    gssk.step(instance, dt);
    const state = gssk.getState(instance); // Float64Array
    editor.updateState(state);
}
```
