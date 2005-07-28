import java.io.DataInput;
import java.io.IOException;

public class ChdescRollback extends Opcode
{
	public ChdescRollback(int chdesc)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_ROLLBACK, "KDB_CHDESC_ROLLBACK", ChdescRollback.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
