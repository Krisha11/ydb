#include <library/cpp/testing/unittest/registar.h>

#include <ydb/library/yql/providers/yt/provider/yql_yt_join_impl.h>
#include <yql/essentials/core/cbo/cbo_optimizer_new.h>
#include <ydb/library/yql/dq/opt/dq_opt_log.h>

namespace NYql {

namespace {

TExprNode::TPtr MakeLabel(const std::vector<TString>& labelStrings, TExprContext& ctx) {
    TVector<TExprNodePtr> label; label.reserve(labelStrings.size());

    auto position = ctx.AppendPosition({});
    for (auto& str : labelStrings) {
        label.emplace_back(ctx.NewAtom(position, str));
    }

    return Build<TCoAtomList>(ctx, position)
            .Add(label)
            .Done()
        .Ptr();
}

TYtJoinNodeOp::TPtr MakeOp(const std::vector<TString>& leftLabel, const std::vector<TString>& rightLabel, TVector<TString>&& scope, TExprContext& ctx) {
    auto op = MakeIntrusive<TYtJoinNodeOp>();
    auto position = ctx.AppendPosition({});
    op->LeftLabel = MakeLabel(leftLabel, ctx);
    op->RightLabel = MakeLabel(rightLabel, ctx);
    op->JoinKind = ctx.NewAtom(position, "Inner");
    op->Scope = std::move(scope);
    return op;
}

TYtJoinNodeLeaf::TPtr MakeLeaf(const std::vector<TString>& label, TVector<TString>&& scope, ui64 rows, ui64 size, TExprContext& ctx) {
    // fake section
    auto position = ctx.AppendPosition({});

    auto section = Build<TYtSection>(ctx, position)
        .Paths().Build()
        .Settings()
            .Add().Name().Build("Test").Value<TCoAtom>().Build("1").Build()
            .Add().Name().Build("Rows").Value<TCoAtom>().Build(ToString(rows)).Build()
            .Add().Name().Build("Size").Value<TCoAtom>().Build(ToString(size)).Build()
        .Build()
        .Done();

    auto leaf = MakeIntrusive<TYtJoinNodeLeaf>(section, TMaybeNode<TCoLambda>{});
    if (label.size() == 1) {
        leaf->Label = ctx.NewAtom(position, label.front());
    } else {
        leaf->Label = MakeLabel(label, ctx);
    }
    leaf->Scope = std::move(scope);
    return leaf;
}

} // namespace

Y_UNIT_TEST_SUITE(TYqlCBO) {

Y_UNIT_TEST(OrderJoinsDoesNothingWhenCBODisabled) {
    const TString cluster("ut_cluster");
    TTypeAnnotationContext typeCtx;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    TYtJoinNodeOp::TPtr tree = nullptr;
    TYtJoinNodeOp::TPtr optimizedTree;

    TExprContext ctx;

    optimizedTree = OrderJoins(tree, state, cluster, ctx);
    UNIT_ASSERT_VALUES_EQUAL(tree, optimizedTree);
}

Y_UNIT_TEST(NonReordable) {
    auto left = std::make_shared<TRelOptimizerNode>("a", TOptimizerStatistics());
    auto right = std::make_shared<TRelOptimizerNode>("a", TOptimizerStatistics());

    TVector<NDq::TJoinColumn> leftKeys = {NDq::TJoinColumn{"a", "b"}};
    TVector<NDq::TJoinColumn> rightKeys = {NDq::TJoinColumn{"a","c"}};

    auto root = std::make_shared<TJoinOptimizerNode>(
        left, right, leftKeys, rightKeys, EJoinKind::InnerJoin, EJoinAlgoType::GraceJoin, false, false, true);
    TBaseProviderContext optCtx;
    std::unique_ptr<IOptimizerNew> opt = std::unique_ptr<IOptimizerNew>(NDq::MakeNativeOptimizerNew(optCtx, 1024));
    auto result = opt->JoinSearch(root);

    // Join tree is built from scratch with DPhyp, check the structure by comapring with Stats 
    UNIT_ASSERT(root->LeftArg->Kind == RelNodeType);
    UNIT_ASSERT(
        &std::static_pointer_cast<TRelOptimizerNode>(root->LeftArg)->Stats == &left->Stats
    );

    UNIT_ASSERT(root->RightArg->Kind == RelNodeType);
    UNIT_ASSERT(
        &std::static_pointer_cast<TRelOptimizerNode>(root->RightArg)->Stats == &right->Stats
    );
}

Y_UNIT_TEST(BuildOptimizerTree2Tables) {
    const TString cluster("ut_cluster");
    TTypeAnnotationContext typeCtx;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"},  100000, 12333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n"}, 1000, 1233, exprCtx);

    std::shared_ptr<IBaseOptimizerNode> resultTree;
    std::shared_ptr<IProviderContext> resultCtx;
    TOptimizerLinkSettings linkSettings;
    BuildOptimizerJoinTree(state, cluster, resultTree, resultCtx, linkSettings, tree, exprCtx);

    UNIT_ASSERT(resultTree->Kind == JoinNodeType);
    auto root = std::static_pointer_cast<TJoinOptimizerNode>(resultTree);
    UNIT_ASSERT(root->LeftArg->Kind == RelNodeType);
    UNIT_ASSERT(root->RightArg->Kind == RelNodeType);

    auto left = std::static_pointer_cast<TRelOptimizerNode>(root->LeftArg);
    auto right = std::static_pointer_cast<TRelOptimizerNode>(root->RightArg);

    UNIT_ASSERT_VALUES_EQUAL(left->Label, "c");
    UNIT_ASSERT_VALUES_EQUAL(right->Label, "n");
    UNIT_ASSERT_VALUES_EQUAL(left->Stats.Nrows, 100000);
    UNIT_ASSERT_VALUES_EQUAL(right->Stats.Nrows, 1000);
}

Y_UNIT_TEST(BuildOptimizerTree2TablesComplexLabel) {
    const TString cluster("ut_cluster");
    TTypeAnnotationContext typeCtx;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n", "e"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"}, 1000000, 1233333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n", "e"}, 10000, 12333, exprCtx);

    std::shared_ptr<IBaseOptimizerNode> resultTree;
    std::shared_ptr<IProviderContext> resultCtx;
    TOptimizerLinkSettings linkSettings;
    BuildOptimizerJoinTree(state, cluster, resultTree, resultCtx, linkSettings, tree, exprCtx);

    UNIT_ASSERT(resultTree->Kind == JoinNodeType);
    auto root = std::static_pointer_cast<TJoinOptimizerNode>(resultTree);
    UNIT_ASSERT(root->LeftArg->Kind == RelNodeType);
    UNIT_ASSERT(root->RightArg->Kind == RelNodeType);

    auto left = std::static_pointer_cast<TRelOptimizerNode>(root->LeftArg);
    auto right = std::static_pointer_cast<TRelOptimizerNode>(root->RightArg);

    UNIT_ASSERT_VALUES_EQUAL(left->Label, "c");
    UNIT_ASSERT_VALUES_EQUAL(right->Label, "n");
    UNIT_ASSERT_VALUES_EQUAL(left->Stats.Nrows, 1000000);
    UNIT_ASSERT_VALUES_EQUAL(right->Stats.Nrows, 10000);
}

Y_UNIT_TEST(BuildYtJoinTree2Tables) {
    const TString cluster("ut_cluster");
    TTypeAnnotationContext typeCtx;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"},  100000, 12333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n"}, 1000, 1233, exprCtx);

    std::shared_ptr<IBaseOptimizerNode> resultTree;
    std::shared_ptr<IProviderContext> resultCtx;
    TOptimizerLinkSettings linkSettings;
    BuildOptimizerJoinTree(state, cluster, resultTree, resultCtx, linkSettings, tree, exprCtx);

    auto joinTree = BuildYtJoinTree(resultTree, exprCtx, {});

    UNIT_ASSERT(AreSimilarTrees(joinTree, tree));
}

Y_UNIT_TEST(BuildYtJoinTree2TablesForceMergeJoib) {
    const TString cluster("ut_cluster");
    TTypeAnnotationContext typeCtx;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"},  100000, 12333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n"}, 1000, 1233, exprCtx);
    tree->LinkSettings.ForceSortedMerge = true;

    std::shared_ptr<IBaseOptimizerNode> resultTree;
    std::shared_ptr<IProviderContext> resultCtx;
    TOptimizerLinkSettings linkSettings;
    BuildOptimizerJoinTree(state, cluster, resultTree, resultCtx, linkSettings, tree, exprCtx);

    auto joinTree = BuildYtJoinTree(resultTree, exprCtx, {});

    UNIT_ASSERT(joinTree == tree);
}

Y_UNIT_TEST(BuildYtJoinTree2TablesComplexLabel) {
    const TString cluster("ut_cluster");
    TTypeAnnotationContext typeCtx;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n", "e"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"}, 1000000, 1233333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n", "e"}, 10000, 12333, exprCtx);

    std::shared_ptr<IBaseOptimizerNode> resultTree;
    std::shared_ptr<IProviderContext> resultCtx;
    TOptimizerLinkSettings linkSettings;
    BuildOptimizerJoinTree(state, cluster, resultTree, resultCtx, linkSettings, tree, exprCtx);
    auto joinTree = BuildYtJoinTree(resultTree, exprCtx, {});

    UNIT_ASSERT(AreSimilarTrees(joinTree, tree));
}

Y_UNIT_TEST(BuildYtJoinTree2TablesTableIn2Rels)
{
    const TString cluster("ut_cluster");
    TTypeAnnotationContext typeCtx;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n", "c"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"}, 1000000, 1233333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n", "c"}, 10000, 12333, exprCtx);

    std::shared_ptr<IBaseOptimizerNode> resultTree;
    std::shared_ptr<IProviderContext> resultCtx;
    TOptimizerLinkSettings linkSettings;
    BuildOptimizerJoinTree(state, cluster, resultTree, resultCtx, linkSettings, tree, exprCtx);
    auto joinTree = BuildYtJoinTree(resultTree, exprCtx, {});

    UNIT_ASSERT(AreSimilarTrees(joinTree, tree));
}

#define ADD_TEST(Name) \
    Y_UNIT_TEST(Name ## _PG) { \
        Name(ECostBasedOptimizerType::PG); \
    } \
    Y_UNIT_TEST(Name ## _Native) { \
        Name(ECostBasedOptimizerType::Native); \
    }

void OrderJoins2Tables(auto optimizerType) {
    const TString cluster("ut_cluster");
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"},  100000, 12333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n"}, 1000, 1233, exprCtx);

    TTypeAnnotationContext typeCtx;
    typeCtx.CostBasedOptimizer = optimizerType;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    auto optimizedTree = OrderJoins(tree, state, cluster, exprCtx, true);
    UNIT_ASSERT(optimizedTree != tree);
    UNIT_ASSERT(optimizedTree->Left);
    UNIT_ASSERT(optimizedTree->Right);
    UNIT_ASSERT(optimizedTree->LeftLabel);
    UNIT_ASSERT(optimizedTree->RightLabel);
    UNIT_ASSERT(optimizedTree->JoinKind);
    UNIT_ASSERT(optimizedTree->LeftLabel->ChildrenSize() == 2);
    UNIT_ASSERT(optimizedTree->RightLabel->ChildrenSize() == 2);
    UNIT_ASSERT_VALUES_EQUAL("c", optimizedTree->LeftLabel->Child(0)->Content());
    UNIT_ASSERT_VALUES_EQUAL("c_nationkey", optimizedTree->LeftLabel->Child(1)->Content());
    UNIT_ASSERT_VALUES_EQUAL("n", optimizedTree->RightLabel->Child(0)->Content());
    UNIT_ASSERT_VALUES_EQUAL("n_nationkey", optimizedTree->RightLabel->Child(1)->Content());
}

ADD_TEST(OrderJoins2Tables)

void OrderJoins2TablesComplexLabel(auto optimizerType)
{
    const TString cluster("ut_cluster");
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n", "e"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"}, 1000000, 1233333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n", "e"}, 10000, 12333, exprCtx);

    TTypeAnnotationContext typeCtx;
    typeCtx.CostBasedOptimizer = optimizerType;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    auto optimizedTree = OrderJoins(tree, state, cluster, exprCtx, true);
    UNIT_ASSERT(optimizedTree != tree);
}

ADD_TEST(OrderJoins2TablesComplexLabel)

void OrderJoins2TablesTableIn2Rels(auto optimizerType)
{
    const TString cluster("ut_cluster");
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n", "e"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"}, 1000000, 1233333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n", "c"}, 10000, 12333, exprCtx);

    TTypeAnnotationContext typeCtx;
    typeCtx.CostBasedOptimizer = optimizerType;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    auto optimizedTree = OrderJoins(tree, state, cluster, exprCtx, true);
    UNIT_ASSERT(optimizedTree != tree);
}

ADD_TEST(OrderJoins2TablesTableIn2Rels)

Y_UNIT_TEST(OrderLeftJoin)
{
    const TString cluster("ut_cluster");
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"}, 1000000, 1233333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n"}, 10000, 12333, exprCtx);
    tree->JoinKind = exprCtx.NewAtom(exprCtx.AppendPosition({}), "Left");

    TTypeAnnotationContext typeCtx;
    typeCtx.CostBasedOptimizer = ECostBasedOptimizerType::PG;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    auto optimizedTree = OrderJoins(tree, state, cluster, exprCtx, true);
    UNIT_ASSERT(optimizedTree != tree);
    UNIT_ASSERT_STRINGS_EQUAL("Left", optimizedTree->JoinKind->Content());
}

Y_UNIT_TEST(UnsupportedJoin)
{
    const TString cluster("ut_cluster");
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"}, 1000000, 1233333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n"}, 10000, 12333, exprCtx);
    tree->JoinKind = exprCtx.NewAtom(exprCtx.AppendPosition({}), "RightSemi");

    TTypeAnnotationContext typeCtx;
    typeCtx.CostBasedOptimizer = ECostBasedOptimizerType::PG;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    auto optimizedTree = OrderJoins(tree, state, cluster, exprCtx, true);
    UNIT_ASSERT(optimizedTree == tree);
}

Y_UNIT_TEST(OrderJoinSinglePass) {
    const TString cluster("ut_cluster");
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"}, 1000000, 1233333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n"}, 10000, 12333, exprCtx);
    tree->JoinKind = exprCtx.NewAtom(exprCtx.AppendPosition({}), "Left");

    TTypeAnnotationContext typeCtx;
    typeCtx.CostBasedOptimizer = ECostBasedOptimizerType::PG;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    auto optimizedTree = OrderJoins(tree, state, cluster, exprCtx, true);
    UNIT_ASSERT(optimizedTree != tree);
    UNIT_ASSERT(optimizedTree->CostBasedOptPassed);
}

Y_UNIT_TEST(OrderJoinsDoesNothingWhenCBOAlreadyPassed) {
    const TString cluster("ut_cluster");
    TExprContext exprCtx;
    auto tree = MakeOp({"c", "c_nationkey"}, {"n", "n_nationkey"}, {"c", "n"}, exprCtx);
    tree->Left = MakeLeaf({"c"}, {"c"}, 1000000, 1233333, exprCtx);
    tree->Right = MakeLeaf({"n"}, {"n"}, 10000, 12333, exprCtx);
    tree->JoinKind = exprCtx.NewAtom(exprCtx.AppendPosition({}), "Left");
    tree->CostBasedOptPassed = true;

    TTypeAnnotationContext typeCtx;
    typeCtx.CostBasedOptimizer = ECostBasedOptimizerType::PG;
    TYtState::TPtr state = MakeIntrusive<TYtState>(&typeCtx);
    auto optimizedTree = OrderJoins(tree, state, cluster, exprCtx, true);
    UNIT_ASSERT(optimizedTree == tree);
}

} // Y_UNIT_TEST_SUITE(TYqlCBO)

} // namespace NYql
