#include "ntuple_test.hxx"
#include "SimpleCollectionProxy.hxx"

TEST(RNTuple, TClassDefaultTemplateParameterInner)
{
   FileRaii fileGuard("test_ntuple_default_template_parameter_inner.root");

   {
      auto model = RNTupleModel::Create();
      model->AddField(RFieldBase::Create("f3", "DataVector<int>::Inner").Unwrap());
      auto writer = RNTupleWriter::Recreate(std::move(model), "ntpl", fileGuard.GetPath());
   }

   auto reader = RNTupleReader::Open("ntpl", fileGuard.GetPath());
   EXPECT_EQ(0u, reader->GetNEntries());

   const auto &desc = reader->GetDescriptor();
   EXPECT_EQ("DataVector<std::int32_t,double>::Inner", desc.GetFieldDescriptor(desc.FindFieldId("f3")).GetTypeName());
}
