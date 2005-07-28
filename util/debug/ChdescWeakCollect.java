import java.io.DataInput;
import java.io.IOException;

public class ChdescWeakCollect extends Opcode
{
	public ChdescWeakCollect(int chdesc)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_WEAK_COLLECT, "KDB_CHDESC_WEAK_COLLECT", ChdescWeakCollect.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
