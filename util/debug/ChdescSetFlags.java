import java.io.DataInput;
import java.io.IOException;

public class ChdescSetFlags extends Opcode
{
	private final int chdesc, flags;
	
	public ChdescSetFlags(int chdesc, int flags)
	{
		this.chdesc = chdesc;
		this.flags = flags;
	}
	
	public void applyTo(SystemState state)
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
