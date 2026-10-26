// Patchy — GraphModel unit tests (JUCE UnitTest framework)

#include <juce_core/juce_core.h>
#include "../Source/GraphModel.h"

// ─────────────────────────────────────────────────────────────────────────────
class GraphModelTests : public juce::UnitTest
{
public:
    GraphModelTests() : juce::UnitTest ("GraphModel", "Patchy") {}

    void runTest() override
    {
        testAddNode();
        testRemoveNode();
        testAddConnection();
        testRemoveConnection();
        testClear();
        testPortsForType();
        testAudioMonitorObserver();
        testLegacyAudioMonitorMigration();
        testCycleDetection();
        testSerialisationRoundtrip();
        testWraps();
        testWrapPins();
        testWrapMembers();
    }

private:
    void testAudioMonitorObserver()
    {
        beginTest ("Audio Monitor is an input-only observer in new graphs");
        auto g = makeGraph();
        const auto source = g.addNode (26, 0.f, 0.f).id;
        const auto monitor = g.addNode (6, 100.f, 0.f).id;
        const auto output = g.addNode (4, 200.f, 0.f).id;
        const auto* node = g.findNode (monitor);
        expect (node != nullptr);
        if (node == nullptr) return;
        expectEquals ((int) node->ports.size(), 1);
        if (node->ports.size() != 1) return;
        expect (node->ports.front().direction == PortDirection::Input);
        expect (node->ports.front().type == PortType::Audio);
        expectEquals (node->ports.front().id, monitor + "_Audio In_in");
        expect (g.addConnection (source, source + "_Audio Out_out", monitor, monitor + "_Audio In_in") != nullptr);
        expect (g.addConnection (monitor, monitor + "_Audio Out_out", output, output + "_Audio In_in") == nullptr);
        expectEquals (g.getConnectionCount(), 1);
    }

    void testLegacyAudioMonitorMigration()
    {
        beginTest ("Legacy monitor chains, forks and cycles become deduplicated observer branches");
        auto g = makeGraph();
        int notifications = 0;
        g.onChange = [&] { ++notifications; };
        g.suspendNotifications();
        const auto sourceA = g.restoreNode ("source-a", 26, 0.f, 0.f).id;
        const auto sourceB = g.restoreNode ("source-b", 26, 0.f, 100.f).id;
        const auto monitorA = g.restoreNode ("monitor-a", 6, 100.f, 0.f).id;
        const auto monitorB = g.restoreNode ("monitor-b", 6, 200.f, 0.f).id;
        const auto isolated = g.restoreNode ("isolated-monitor", 6, 200.f, 100.f).id;
        const auto outputA = g.restoreNode ("output-a", 4, 300.f, 0.f).id;
        const auto outputB = g.restoreNode ("output-b", 4, 300.f, 100.f).id;
        const juce::String settings = R"({"layout":"stacked","selectedChannels":[0,2],"timeWindowMs":25,"customName":"Scope A"})";
        g.findNode (monitorA)->settingsJson = settings;
        g.findNode (monitorA)->label = "Saved Monitor";
        g.findNode (monitorA)->customName = "Saved custom name";
        g.findNode (monitorA)->disabled = true;
        const auto connect = [&](const juce::String& source, const juce::String& target)
        { return g.addConnection (source, source + "_Audio Out_out", target, target + "_Audio In_in"); };

        // Restore outputs before inputs deliberately: migration must not depend
        // on connection order in a legacy file or undo snapshot.
        connect (monitorB, outputA);
        connect (monitorB, outputB);
        connect (monitorA, monitorB);
        connect (monitorB, monitorA); // Monitor-only cycle, with two real sources.
        connect (isolated, isolated); // An undriven cycle must terminate too.
        connect (isolated, outputA);
        connect (sourceA, monitorA);
        connect (sourceB, monitorA);
        connect (sourceA, monitorB); // Duplicate path through monitorA.
        const auto directId = connect (sourceA, outputA)->id;
        const auto wrap = g.addWrap ("Scopes", juce::StringArray { monitorA, monitorB },
            juce::StringArray { monitorA + "_Audio Out_out", monitorB + "_Audio In_in" }, 10.f, 20.f).id;
        const auto legacySnapshot = g.toVar();
        g.resumeNotifications();
        expectEquals (notifications, 1);

        const auto hasRoute = [&](const GraphModel& model, const juce::String& source, const juce::String& target)
        {
            return std::any_of (model.getConnections().begin(), model.getConnections().end(), [&](const Connection& connection)
                { return connection.sourceNodeId == source && connection.sourcePortId == source + "_Audio Out_out"
                      && connection.targetNodeId == target && connection.targetPortId == target + "_Audio In_in"; });
        };
        const auto expectCanonical = [&](const GraphModel& model)
        {
            for (const auto& node : model.getNodes())
                if (node.nodeType == 6)
                {
                    expectEquals ((int) node.ports.size(), 1);
                    if (! node.ports.empty()) expect (node.ports.front().direction == PortDirection::Input);
                    for (const auto& connection : model.getConnections())
                        expect (connection.sourceNodeId != node.id, "An observer must not retain outgoing routes");
                }
        };
        expectEquals (g.getConnectionCount(), 8);
        for (const auto& source : { sourceA, sourceB })
            for (const auto& target : { monitorA, monitorB, outputA, outputB })
                expect (hasRoute (g, source, target));
        expect (g.hasConnection (directId), "Existing direct connections keep their IDs");
        expectCanonical (g);
        expectEquals (g.findNode (monitorA)->settingsJson, settings);
        expectEquals (g.findNode (monitorA)->label, juce::String ("Saved Monitor"));
        expectEquals (g.findNode (monitorA)->customName, juce::String ("Saved custom name"));
        expect (g.findNode (monitorA)->disabled);
        expectEquals (g.findWrap (wrap)->ports.size(), 1);
        expectEquals (g.findWrap (wrap)->ports[0], monitorB + "_Audio In_in");

        beginTest ("Monitor migration is idempotent and undo/redo stay input-only");
        const auto normalized = juce::JSON::toString (g.toVar());
        g.normalizeLegacyMonitorConnections();
        expectEquals (juce::JSON::toString (g.toVar()), normalized);
        g.pushSnapshot();
        g.removeNode (outputB);
        expectEquals (g.getConnectionCount(), 6);
        expect (g.undo());
        expectEquals (g.getConnectionCount(), 8);
        expectCanonical (g);
        expectEquals (g.findNode (monitorA)->settingsJson, settings);
        expect (g.redo());
        expectEquals (g.getConnectionCount(), 6);
        expectCanonical (g);

        beginTest ("Restoring an old undo snapshot also normalizes legacy monitor routes");
        auto restored = makeGraph();
        restored.pushExistingSnapshot (legacySnapshot);
        expect (restored.undo());
        expectEquals (restored.getConnectionCount(), 8);
        expectCanonical (restored);
        expectEquals (restored.findNode (monitorA)->settingsJson, settings);
        expect (restored.redo());
        expectEquals (restored.getNodeCount(), 0);
        expect (restored.undo());
        expectEquals (restored.getConnectionCount(), 8);
        expectCanonical (restored);
    }

    // ── helpers ───────────────────────────────────────────────────────────────
    GraphModel makeGraph()
    {
        GraphModel g;
        g.onChange = [](){};   // suppress callbacks during tests
        return g;
    }

    // ── tests ─────────────────────────────────────────────────────────────────

    void testAddNode()
    {
        beginTest ("addNode creates node with correct type and ports");

        auto g = makeGraph();

        // MIDI In (type 1) — should have 1 MIDI Out port
        juce::String id1 = g.addNode (1, 100.f, 100.f).id;
        expectEquals (g.getNodes().size(), (size_t) 1);
        expectEquals (g.getNodes()[0].nodeType, 1);
        expect (!id1.isEmpty());

        // Audio Out (type 4) — should have 1 Audio In port
        juce::String id2 = g.addNode (4, 200.f, 200.f).id;
        expectEquals (g.getNodes().size(), (size_t) 2);
        expectEquals (g.getNodes()[1].nodeType, 4);

        // IDs must be unique
        expect (id1 != id2);
    }

    void testRemoveNode()
    {
        beginTest ("removeNode removes node and its connections");

        auto g = makeGraph();
        // Copy IDs immediately — vector may reallocate on second addNode
        juce::String id1 = g.addNode (1, 0.f, 0.f).id;
        juce::String portId1 = g.getNodes()[0].ports.empty() ? "" : g.getNodes()[0].ports[0].id;
        juce::String id2 = g.addNode (2, 100.f, 0.f).id;
        juce::String portId2 = g.getNodes()[1].ports.empty() ? "" : g.getNodes()[1].ports[0].id;

        // Add a connection between them
        if (portId1.isNotEmpty() && portId2.isNotEmpty())
            g.addConnection (id1, portId1, id2, portId2);

        expectEquals (g.getNodes().size(), (size_t) 2);

        bool removed = g.removeNode (id1);
        expect (removed);
        expectEquals (g.getNodes().size(), (size_t) 1);

        // Connection should also be gone
        expectEquals (g.getConnections().size(), (size_t) 0);
    }

    void testAddConnection()
    {
        beginTest ("addConnection creates connection between compatible ports");

        auto g = makeGraph();
        juce::String idIn  = g.addNode (1, 0.f,   0.f).id;
        juce::String idOut = g.addNode (2, 100.f, 0.f).id;

        juce::String srcPort, dstPort;
        for (auto& p : g.getNodes()[0].ports)
            if (p.direction == PortDirection::Output && p.type == PortType::Midi)
                { srcPort = p.id; break; }
        for (auto& p : g.getNodes()[1].ports)
            if (p.direction == PortDirection::Input && p.type == PortType::Midi)
                { dstPort = p.id; break; }

        expect (!srcPort.isEmpty(), "MIDI In node should have MIDI output port");
        expect (!dstPort.isEmpty(), "MIDI Out node should have MIDI input port");

        auto* conn = g.addConnection (idIn, srcPort, idOut, dstPort);
        expect (conn != nullptr);
        expectEquals (g.getConnections().size(), (size_t) 1);
    }

    void testRemoveConnection()
    {
        beginTest ("removeConnection removes existing connection");

        auto g = makeGraph();
        juce::String id1 = g.addNode (1, 0.f, 0.f).id;
        juce::String id2 = g.addNode (2, 100.f, 0.f).id;

        juce::String srcPort, dstPort;
        for (auto& p : g.getNodes()[0].ports)
            if (p.direction == PortDirection::Output) { srcPort = p.id; break; }
        for (auto& p : g.getNodes()[1].ports)
            if (p.direction == PortDirection::Input)  { dstPort = p.id; break; }

        auto* conn = g.addConnection (id1, srcPort, id2, dstPort);
        expect (conn != nullptr);

        bool removed = g.removeConnection (conn->id);
        expect (removed);
        expectEquals (g.getConnections().size(), (size_t) 0);
    }

    void testClear()
    {
        beginTest ("clear removes all nodes and connections");

        auto g = makeGraph();
        g.addNode (1, 0.f, 0.f);
        g.addNode (2, 100.f, 0.f);
        g.addNode (3, 200.f, 0.f);
        expectEquals (g.getNodes().size(), (size_t) 3);

        g.clear();
        expectEquals (g.getNodes().size(), (size_t) 0);
        expectEquals (g.getConnections().size(), (size_t) 0);
    }

    void testPortsForType()
    {
        beginTest ("portsForType returns correct ports for each built-in type");

        // Type 1: MIDI In — should have 1 MIDI output
        auto ports1 = GraphModel::portsForType (1, "test1");
        expect (!ports1.empty());
        expect (std::any_of (ports1.begin(), ports1.end(),
            [](const Port& p){ return p.type == PortType::Midi
                                   && p.direction == PortDirection::Output; }));

        // Type 3: Audio In — should have 1 Audio output
        auto ports3 = GraphModel::portsForType (3, "test3");
        expect (!ports3.empty());
        expect (std::any_of (ports3.begin(), ports3.end(),
            [](const Port& p){ return p.type == PortType::Audio
                                   && p.direction == PortDirection::Output; }));

        // Type 102: Pax Audio — should have audio in + out
        auto ports102 = GraphModel::portsForType (102, "test102");
        expect (!ports102.empty());
    }

    void testCycleDetection()
    {
        beginTest ("addConnection prevents cycles");

        auto g = makeGraph();
        juce::String id1 = g.addNode (1, 0.f,   0.f).id;
        juce::String id2 = g.addNode (2, 100.f, 0.f).id;

        juce::String p1out, p2in;
        for (auto& p : g.getNodes()[0].ports)
            if (p.direction == PortDirection::Output) { p1out = p.id; break; }
        for (auto& p : g.getNodes()[1].ports)
            if (p.direction == PortDirection::Input)  { p2in  = p.id; break; }

        // n1 → n2 should succeed
        if (!p1out.isEmpty() && !p2in.isEmpty())
        {
            auto* c1 = g.addConnection (id1, p1out, id2, p2in);
            expect (c1 != nullptr, "Forward connection should succeed");
        }
    }

    void testSerialisationRoundtrip()
    {
        beginTest ("toVar / restoreNode roundtrip preserves graph structure");

        auto g = makeGraph();
        g.addNode (1, 100.f, 200.f);
        g.addNode (4, 300.f, 400.f);

        // Serialise
        auto state = g.toVar();
        expect (state.isObject());

        // Count nodes in serialised form
        if (auto* obj = state.getDynamicObject())
        {
            auto nodesVar = obj->getProperty ("nodes");
            if (auto* arr = nodesVar.getArray())
                expectEquals ((int) arr->size(), 2);
        }

        // Restore into a fresh graph
        GraphModel g2;
        g2.onChange = [](){};

        if (auto* obj = state.getDynamicObject())
        {
            auto nodesArr = obj->getProperty ("nodes");
            if (auto* arr = nodesArr.getArray())
            {
                for (auto& nv : *arr)
                {
                    if (auto* nd = nv.getDynamicObject())
                    {
                        g2.restoreNode (
                            nd->getProperty ("id").toString(),
                            (int)   nd->getProperty ("nodeType"),
                            (float) nd->getProperty ("x"),
                            (float) nd->getProperty ("y"),
                            nd->getProperty ("paxName").toString());
                    }
                }
            }
        }

        expectEquals (g2.getNodes().size(), (size_t) 2);
        expectEquals (g2.getNodes()[0].nodeType, g.getNodes()[0].nodeType);
        expectEquals (g2.getNodes()[1].nodeType, g.getNodes()[1].nodeType);
    }

    // v0.0.930 — wraps: UI/project state kept in GraphModel
    void testWraps()
    {
        beginTest ("wraps: add, one-level membership, node removal, undo snapshot round trip");

        auto g = makeGraph();
        const auto a = g.addNode (5, 0.f, 0.f).id;     // MIDI Monitor
        const auto b = g.addNode (28, 200.f, 0.f).id;  // MIDI Morpher
        const auto c = g.addNode (5, 400.f, 0.f).id;

        auto& p1 = g.addWrap ("FIRST", juce::StringArray { a, b }, juce::StringArray { "x_MIDI In_in" }, 10.f, 20.f);
        const auto p1Id = p1.id;
        expectEquals ((int) g.getWraps().size(), 1);
        expectEquals (g.findWrap (p1Id)->nodeIds.size(), 2);

        // A node joining another wrap leaves the first one (one level, one wrap per node)
        const auto p2Id = g.addWrap ("SECOND", juce::StringArray { b, c }, {}, 0.f, 0.f).id;
        expectEquals (g.findWrap (p1Id)->nodeIds.size(), 1, "b moved out of FIRST");
        expectEquals (g.findWrap (p2Id)->nodeIds.size(), 2);

        // Undo snapshot round trip keeps wraps (name, members, ports, position, open)
        g.findWrap (p1Id)->open = true;
        g.pushSnapshot();
        g.removeWrap (p2Id);
        expectEquals ((int) g.getWraps().size(), 1);
        expect (g.undo());
        expectEquals ((int) g.getWraps().size(), 2, "undo brings the wrap back");
        auto* r1 = g.findWrap (p1Id);
        expect (r1 != nullptr);
        if (r1 != nullptr)
        {
            expectEquals (r1->name, juce::String ("FIRST"));
            expectEquals (r1->ports[0], juce::String ("x_MIDI In_in"));
            expectEquals ((int) r1->x, 10);
            expect (r1->open);
        }

        // Removing the last member removes the wrap
        g.removeNode (a);
        expect (g.findWrap (p1Id) == nullptr, "empty wrap removed");
        expectEquals ((int) g.getWraps().size(), 1);

        // toVar carries wraps; restoreWraps drops members that no longer exist
        auto v = g.toVar();
        expect (v.getProperty ("wraps", {}).isArray());
        g.removeNode (c);
        g.restoreWraps (v.getProperty ("wraps", {}));
        expectEquals ((int) g.getWraps().size(), 1);
        expectEquals (g.getWraps()[0].nodeIds.size(), 1, "c dropped, b kept");

        // v0.0.936 — files saved before the rename keep their wraps under "packs"
        {
            auto* old = new juce::DynamicObject();
            old->setProperty ("packs", v.getProperty ("wraps", {}));
            juce::var oldVar (old);
            expect (GraphModel::wrapsOf (*old).isArray(), "\"packs\" read as wraps");
            g.restoreWraps (GraphModel::wrapsOf (*old));
            expectEquals ((int) g.getWraps().size(), 1, "old file: wrap restored");

            // the old default name follows the rename; a chosen name doesn't
            auto* o2 = new juce::DynamicObject();
            juce::var o2v (o2);
            juce::Array<juce::var> arr;
            for (const auto* nm : { "PACK", "Drums" })
            {
                auto* w = new juce::DynamicObject();
                w->setProperty ("id", juce::Uuid().toString());
                w->setProperty ("name", nm);
                w->setProperty ("nodeIds", juce::Array<juce::var> { g.getWraps()[0].nodeIds[0] });
                arr.add (juce::var (w));
            }
            o2->setProperty ("packs", arr);
            g.restoreWraps (GraphModel::wrapsOf (*o2));
            expectEquals (g.getWraps()[0].name, juce::String ("WRAP"));
            expectEquals (g.getWraps()[1].name, juce::String ("Drums"));
        }

        // Older snapshots without "wraps" → none
        g.restoreWraps (juce::var());
        expectEquals ((int) g.getWraps().size(), 0);
    }

    void testWrapMembers()
    {
        beginTest ("wrap members (v0.0.935): join / leave, one level, pins pruned, empty wrap removed");

        auto g = makeGraph();
        const auto a = g.addNode (5, 0.f, 0.f).id;
        const auto b = g.addNode (5, 0.f, 0.f).id;
        const auto c = g.addNode (5, 0.f, 0.f).id;
        const auto d = g.addNode (5, 0.f, 0.f).id;
        const auto pId = g.addWrap ("P", juce::StringArray { a, b }, {}, 0.f, 0.f).id;
        const auto qId = g.addWrap ("Q", juce::StringArray { c, d }, {}, 0.f, 0.f).id;
        g.findWrap (qId)->pins = { { c, 0, "" }, { d, 0, "" } };

        // c joins P: leaves Q (its pin there goes), Q keeps d
        expect (g.changeWrapMembers (pId, { c }, {}, { "x_MIDI In_in" }));
        expectEquals (g.findWrap (pId)->nodeIds.size(), 3);
        expectEquals (g.findWrap (pId)->ports[0], juce::String ("x_MIDI In_in"));
        expectEquals (g.findWrap (qId)->nodeIds.size(), 1);
        expectEquals ((int) g.findWrap (qId)->pins.size(), 1, "c's pin left Q with it");
        expectEquals ((int) g.findWrap (pId)->pins.size(), 1, "... and came along into P");
        expectEquals (g.findWrap (pId)->pins[0].nodeId, c);

        // merging a whole wrap: all of Q's nodes join P → Q empty → gone
        const auto e = g.addNode (5, 0.f, 0.f).id;
        const auto rId = g.addWrap ("R", juce::StringArray { e }, {}, 0.f, 0.f).id;
        g.findWrap (rId)->pins = { { e, 0, "Drive" } };
        expect (g.changeWrapMembers (pId, { e }, {}, { "x_MIDI In_in" }));
        expect (g.findWrap (rId) == nullptr, "merged wrap removed");
        expectEquals ((int) g.findWrap (pId)->pins.size(), 2);
        expectEquals (g.findWrap (pId)->pins[1].label, juce::String ("Drive"));
        expect (g.changeWrapMembers (pId, {}, { e }, { "x_MIDI In_in" }));   // back out (keeps the rest of this test as it was)

        // nothing to do → false, nothing changed
        expect (! g.changeWrapMembers (pId, { a }, {}, {}));
        expectEquals (g.findWrap (pId)->ports.size(), 1);

        // a leaves P
        expect (g.changeWrapMembers (pId, {}, { a }, {}));
        expectEquals (g.findWrap (pId)->nodeIds.size(), 2);

        // d leaves Q → Q empty → gone
        expect (g.changeWrapMembers (qId, {}, { d }, {}));
        expect (g.findWrap (qId) == nullptr);

        // unknown nodes are ignored
        expect (! g.changeWrapMembers (pId, { "nope" }, {}, {}));
    }

    void testWrapPins()
    {
        beginTest ("wrap pins (v0.0.932): save / restore, undo, pruned when a node leaves");

        auto g = makeGraph();
        const auto a = g.addNode (5, 0.f, 0.f).id;
        const auto b = g.addNode (5, 200.f, 0.f).id;
        const auto pId = g.addWrap ("P", juce::StringArray { a, b }, {}, 0.f, 0.f).id;

        g.pushSnapshot();
        g.findWrap (pId)->pins = { { a, 0, "Drive" }, { b, 2, "" } };

        // toVar → restoreWraps keeps pins, in order, with labels
        auto v = g.toVar();
        g.restoreWraps (v.getProperty ("wraps", {}));
        auto* p = g.findWrap (pId);
        expect (p != nullptr);
        if (p == nullptr) return;
        expectEquals ((int) p->pins.size(), 2);
        expectEquals (p->pins[0].label, juce::String ("Drive"));
        expectEquals (p->pins[1].index, 2);

        // Undo goes back to no pins
        expect (g.undo());
        expectEquals ((int) g.findWrap (pId)->pins.size(), 0, "undo removes the pins");
        expect (g.redo());
        expectEquals ((int) g.findWrap (pId)->pins.size(), 2, "redo brings them back");

        // A node leaving (removed, or moved to another wrap) takes its pins with it
        g.removeNode (a);
        expectEquals ((int) g.findWrap (pId)->pins.size(), 1);
        const auto c = g.addNode (5, 400.f, 0.f).id;
        g.addWrap ("Q", juce::StringArray { b, c }, {}, 0.f, 0.f);   // b leaves P → P empty → gone
        expect (g.findWrap (pId) == nullptr);

        // Pins of a node that no longer exists are dropped on restore
        auto d = g.addNode (5, 0.f, 0.f).id;
        auto e = g.addNode (5, 0.f, 0.f).id;
        auto& r = g.addWrap ("R", juce::StringArray { d, e }, {}, 0.f, 0.f);
        r.pins = { { d, 0, "" }, { e, 0, "" } };
        auto v2 = g.toVar();
        g.removeNode (e);
        g.restoreWraps (v2.getProperty ("wraps", {}));
        expectEquals ((int) g.getWraps().back().pins.size(), 1, "pin of the missing node dropped");
    }
};

// Register the test suite
static GraphModelTests graphModelTests;
