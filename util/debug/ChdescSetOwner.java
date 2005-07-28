import java.io.DataInput;
import java.io.IOException;

public class ChdescSetOwner extends Opcode
{
	public ChdescSetOwner(int chdesc, int owner)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SET_OWNER, "KDB_CHDESC_SET_OWNER", ChdescSetOwner.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("owner", 4);
		return factory;
	}
}
