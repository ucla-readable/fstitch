import java.io.DataInput;
import java.io.IOException;

public class ChdescInfoModule extends Module
{
	public ChdescInfoModule(DataInput input) throws BadInputException, IOException
	{
		super(input, KDB_MODULE_CHDESC_INFO);
		
		addFactory(ChdescMove.getFactory(input));
		addFactory(ChdescSatisfy.getFactory(input));
		addFactory(ChdescWeakCollect.getFactory(input));
		addFactory(ChdescRollbackCollection.getFactory(input));
		addFactory(ChdescApplyCollection.getFactory(input));
		addFactory(ChdescOrderDestroy.getFactory(input));
		addFactory(ChdescDetachDependencies.getFactory(input));
		addFactory(ChdescDetachDependents.getFactory(input));
		addFactory(ChdescOverlapAttach.getFactory(input));
		addFactory(ChdescOverlapMultiattach.getFactory(input));
		addFactory(ChdescDuplicate.getFactory(input));
		addFactory(ChdescSplit.getFactory(input));
		addFactory(ChdescMerge.getFactory(input));
	}
}
