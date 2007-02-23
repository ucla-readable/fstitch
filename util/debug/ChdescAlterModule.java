import java.io.IOException;

public class ChdescAlterModule extends Module
{
	public ChdescAlterModule(CountingDataInput input) throws BadInputException, IOException
	{
		super(input, KDB_MODULE_CHDESC_ALTER);
		
		addFactory(ChdescCreateNoop.getFactory(input));
		addFactory(ChdescCreateBit.getFactory(input));
		addFactory(ChdescCreateByte.getFactory(input));
		addFactory(ChdescConvertNoop.getFactory(input));
		addFactory(ChdescConvertBit.getFactory(input));
		addFactory(ChdescConvertByte.getFactory(input));
		addFactory(ChdescRewriteByte.getFactory(input));
		addFactory(ChdescApply.getFactory(input));
		addFactory(ChdescRollback.getFactory(input));
		addFactory(ChdescSetFlags.getFactory(input));
		addFactory(ChdescClearFlags.getFactory(input));
		addFactory(ChdescDestroy.getFactory(input));
		addFactory(ChdescAddBefore.getFactory(input));
		addFactory(ChdescAddAfter.getFactory(input));
		addFactory(ChdescRemBefore.getFactory(input));
		addFactory(ChdescRemAfter.getFactory(input));
		addFactory(ChdescWeakRetain.getFactory(input));
		addFactory(ChdescWeakForget.getFactory(input));
		addFactory(ChdescSetOffset.getFactory(input));
		addFactory(ChdescSetXor.getFactory(input));
		addFactory(ChdescSetLength.getFactory(input));
		addFactory(ChdescSetBlock.getFactory(input));
		addFactory(ChdescSetOwner.getFactory(input));
		addFactory(ChdescSetFreePrev.getFactory(input));
		addFactory(ChdescSetFreeNext.getFactory(input));
		addFactory(ChdescSetFreeHead.getFactory(input));
	}
}
