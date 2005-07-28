import java.io.DataInput;
import java.io.IOException;

public class ChdescRollbackCollection extends Opcode
{
	public ChdescRollbackCollection(int count, int chdescs, int order)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_ROLLBACK_COLLECTION, "KDB_CHDESC_ROLLBACK_COLLECTION", ChdescRollbackCollection.class);
		factory.addParameter("count", 4);
		factory.addParameter("chdescs", 4);
		factory.addParameter("order", 4);
		return factory;
	}
}
