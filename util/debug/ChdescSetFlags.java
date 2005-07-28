import java.io.DataInput;
import java.io.IOException;

public class ChdescSetFlags extends Opcode
{
	public ChdescSetFlags(int chdesc, int flags)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SET_FLAGS, "KDB_CHDESC_SET_FLAGS", ChdescSetFlags.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("flags", 4);
		return factory;
	}
}
