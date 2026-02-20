import json

def generate_supply_chain():
    nodes = []
    edges = []

    # 6 tiers, 5 nodes each = 30 nodes
    tiers = {
        "producer": 5,
        "processor": 5,
        "distributor": 5,
        "retailer": 5,
        "consumer": 5,
        "recycler": 5
    }

    # Create nodes
    for tier, count in tiers.items():
        for i in range(1, count + 1):
            node_id = f"{tier}_{i}"
            nodes.append({
                "id": node_id,
                "type": "storage",
                "value": 100.0 if tier == "producer" else 10.0
            })

    # Create edges (the supply chain)
    # 1. External supply to producers
    for i in range(1, 6):
        # We need a source. Let's add one external source and one sink to make it 32 nodes?
        # Or make producers their own sources?
        # The prompt said "30 nodes". I'll stick to 30.
        # I'll use a constant flow into producers from nowhere?
        # Wait, in GSSK, edges must have origin and target.
        # If I want external input, I need a source node.
        pass

    # Actually, let's have:
    # 5 Producers
    # 5 Processors
    # 5 Distributors
    # 5 Retailers
    # 5 Consumers
    # 4 Recyclers
    # 1 Global Source
    # Total = 30 nodes.

    nodes = []
    # 1 Global Source
    nodes.append({"id": "global_source", "type": "source", "value": 10.0}) # Constant supply rate

    # 5 Producers
    for i in range(1, 6):
        nodes.append({"id": f"prod_{i}", "type": "storage", "value": 50.0})
        # Edge from source to producer
        edges.append({
            "id": f"supply_{i}",
            "origin": "global_source",
            "target": f"prod_{i}",
            "logic": "constant",
            "params": {"k": 2.0}
        })

    # 5 Processors
    for i in range(1, 6):
        nodes.append({"id": f"proc_{i}", "type": "storage", "value": 20.0})
        # Take from corresponding producer
        edges.append({
            "id": f"process_{i}",
            "origin": f"prod_{i}",
            "target": f"proc_{i}",
            "logic": "linear",
            "params": {"k": 0.1}
        })
        # Cross-link: take from next producer too
        next_prod = (i % 5) + 1
        edges.append({
            "id": f"process_cross_{i}",
            "origin": f"prod_{next_prod}",
            "target": f"proc_{i}",
            "logic": "linear",
            "params": {"k": 0.05}
        })

    # 5 Distributors
    for i in range(1, 6):
        nodes.append({"id": f"dist_{i}", "type": "storage", "value": 10.0})
        edges.append({
            "id": f"distribute_{i}",
            "origin": f"proc_{i}",
            "target": f"dist_{i}",
            "logic": "linear",
            "params": {"k": 0.2}
        })

    # 5 Retailers
    for i in range(1, 6):
        nodes.append({"id": f"retail_{i}", "type": "storage", "value": 5.0})
        edges.append({
            "id": f"sell_{i}",
            "origin": f"dist_{i}",
            "target": f"retail_{i}",
            "logic": "linear",
            "params": {"k": 0.3}
        })

    # 4 Consumers
    for i in range(1, 5):
        nodes.append({"id": f"cons_{i}", "type": "storage", "value": 0.0})
        # Consumption interaction: Retailer + Consumer demand?
        # Let's just use interaction with a constant demand node.
        edges.append({
            "id": f"consume_{i}",
            "origin": f"retail_{i}",
            "target": f"cons_{i}",
            "logic": "interaction",
            "params": {"k": 0.01, "control_node": f"cons_{i}"}
        })
        # Limit consumption
        edges.append({
            "id": f"waste_{i}",
            "origin": f"cons_{i}",
            "target": f"recy_{i}",
            "logic": "linear",
            "params": {"k": 0.05}
        })

    # 5 Retailers (continued) - 5th retailer doesn't have a consumer?
    # Let's just give it a sink.
    edges.append({
        "id": "sell_5_direct",
        "origin": "retail_5",
        "target": "global_sink",
        "logic": "linear",
        "params": {"k": 0.1}
    })

    # 4 Recyclers
    for i in range(1, 5):
        nodes.append({"id": f"recy_{i}", "type": "storage", "value": 0.0})
        # Recycle back to producers
        edges.append({
            "id": f"recycle_back_{i}",
            "origin": f"recy_{i}",
            "target": f"prod_{i}",
            "logic": "linear",
            "params": {"k": 0.1}
        })

    # 30th node: Global Sink
    nodes.append({"id": "global_sink", "type": "sink", "value": 0.0})

    model = {
        "nodes": nodes,
        "edges": edges,
        "config": {
            "t_start": 0.0,
            "t_end": 50.0,
            "dt": 0.05,
            "method": "rk4"
        }
    }

    print(f"Generated model with {len(nodes)} nodes and {len(edges)} edges.")

    with open("examples/supply_chain_30.json", "w") as f:
        json.dump(model, f, indent=4)

if __name__ == "__main__":
    generate_supply_chain()
