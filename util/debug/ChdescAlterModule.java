import java.io.DataInput;
import java.io.IOException;

public class ChdescAlterModule extends Module
{
	public ChdescAlterModule(DataInput input) throws BadInputException, IOException
	{
		super(input, KDB_MODULE_CHDESC_ALTER);
		
		addFactory(ChdescCreateNoop.getFactory(input));
		addFactory(ChdescCreateBit.getFactory(input));
		addFactory(ChdescCreateByte.getFactory(input));
		addFactory(ChdescConvertNoop.getFactory(input));
		addFactory(ChdescConvertBit.getFactory(input));
		addFactory(ChdescConvertByte.getFactory(input));
		addFactory(ChdescApply.getFactory(input));
		addFactory(ChdescRollback.getFactory(input));
		addFactory(ChdescSetFlags.getFactory(input));
		addFactory(ChdescClearFlags.getFactory(input));
		addFactory(ChdescDestroy.getFactory(input));
		addFactory(ChdescAddDependency.getFactory(input));
		addFactory(ChdescAddDependent.getFactory(input));
		addFactory(ChdescRemDependency.getFactory(input));
		addFactory(ChdescRemDependent.getFactory(input));
		addFactory(ChdescWeakRetain.getFactory(input));
		addFactory(ChdescWeakForget.getFactory(input));
		addFactory(ChdescSetBlock.getFactory(input));
		addFactory(ChdescSetOwner.getFactory(input));
		addFactory(ChdescSetFreePrev.getFactory(input));
		addFactory(ChdescSetFreeNext.getFactory(input));
		addFactory(ChdescSetFreeHead.getFactory(input));
	}
}
