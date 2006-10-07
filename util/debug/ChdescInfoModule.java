import java.io.IOException;

public class ChdescInfoModule extends Module
{
	public ChdescInfoModule(CountingDataInput input) throws BadInputException, IOException
	{
		super(input, KDB_MODULE_CHDESC_INFO);
		
		addFactory(ChdescMove.getFactory(input));
		addFactory(ChdescSatisfy.getFactory(input));
		addFactory(ChdescWeakCollect.getFactory(input));
		addFactory(ChdescRollbackCollection.getFactory(input));
		addFactory(ChdescApplyCollection.getFactory(input));
		addFactory(ChdescOrderDestroy.getFactory(input));
		addFactory(ChdescDetachBefores.getFactory(input));
		addFactory(ChdescDetachAfters.getFactory(input));
		addFactory(ChdescOverlapAttach.getFactory(input));
		addFactory(ChdescOverlapMultiattach.getFactory(input));
		addFactory(ChdescDuplicate.getFactory(input));
		addFactory(ChdescSplit.getFactory(input));
		addFactory(ChdescMerge.getFactory(input));
	}
}
