import java.io.DataInput;
import java.io.IOException;

public class ChdescApplyCollection extends Opcode
{
	public ChdescApplyCollection(int count, int chdescs, int order)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_APPLY_COLLECTION, "KDB_CHDESC_APPLY_COLLECTION", ChdescApplyCollection.class);
		factory.addParameter("count", 4);
		factory.addParameter("chdescs", 4);
		factory.addParameter("order", 4);
		return factory;
	}
}
